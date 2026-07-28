#include "VoxelGpuPoolComponent.h"

#include "VoxelGpuWorldGenGraph.h"
#include "VoxelQuadVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshBatch.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "SceneManagement.h"
#include "RHIResourceUtils.h"
#include "Misc/ScopeExit.h"

#include <atomic>

// Out of line because FVoxelGpuPoolBuffers holds two FRDGPooledBuffer
// references through a forward declaration -- this is the one translation unit
// with the complete type, so it is the one place they may be destroyed.
FVoxelGpuPoolBuffers::FVoxelGpuPoolBuffers() = default;
FVoxelGpuPoolBuffers::~FVoxelGpuPoolBuffers() = default;

// Wave S0 (docs/speculative-generation-plan.md §4, executing T0-1): time what
// publication costs, on both threads.
//
// GATES THE CLOCKS, NOT THE COUNTS. The counters below are increments on a path
// that already copies the whole chunk table and runs a sort; the
// FPlatformTime pairs are the part that could plausibly perturb what they
// measure, and RebuildRunBounds' pair sits next to the render thread's gather
// path, so it is the one that most needs to be absent when not asked for.
// Off by default; a leg turns it on, and one leg with it OFF is what proves the
// instrument is not the perturbation.
//
// File scope rather than inside VoxelGpuPoolCull because both use sites need it
// and RebuildRunBounds precedes that namespace's consumers.
static int32 GVoxelPoolPushStats = 0;
static FAutoConsoleVariableRef CVarVoxelPoolPushStats(
	TEXT("voxel.Stream.PoolPushStats"),
	GVoxelPoolPushStats,
	TEXT("1 = time PushUpdatesToProxy's chunk-table copy and BuildChunkRuns on the game thread, ")
	TEXT("and RebuildRunBounds on the render thread, reported by the streaming subsystem's 5s log. ")
	TEXT("Default 0. The deep review prices RebuildRunBounds at ~2-4ms of render-thread CPU PER ")
	TEXT("APPLIED CHUNK and rests the whole batching wave on that estimate; this is what measures it."),
	ECVF_Default);

// S1-1 (docs/speculative-generation-plan.md Wave S1): publish once per frame
// instead of once per mutated chunk.
//
// DEFAULT 0 = BYTE-IDENTICAL TO TODAY. Every mutator still calls
// PushUpdatesToProxy and, with this off, PushUpdatesToProxy still flushes
// immediately, so the flag off is not "batching with depth 0" -- it is the old
// code path, reachable without an FScopedBatch anywhere in it.
//
// What it is worth, measured before it was built
// (docs/measurements/s0-apply-census-2026-07-27.txt): the publication is 98-99%
// of per-apply cost and runs 0.275 ms early in a fill, 2.108 ms under flight,
// once per applied chunk AND once per unload. Batching divides that by
// applies-per-frame.
//
// READ UnmarkQuadsDirty BEFORE FLIPPING THIS. Batching inverts an ordering that
// per-chunk flushes were providing for free, and the failure mode is invisible
// terrain that reports as loaded.
// DEFAULT 1 SINCE 2026-07-27, AND THAT IS A MEASUREMENT
// (docs/measurements/s1-close-2026-07-27.txt).
//
// It is the single largest lever this programme has found: the pooled arm went
// from ~260 chunks/s with 15,032 converged holes to ~795 with holes in single
// digits, and the P0 that framed the whole streaming-perf effort closed with it.
//
// Most of the win is NOT the amortisation it was designed for. Publications fell
// 16.5x and applies rose 3.3x, which stopped the ResultsQueue backing up, which
// took the stale-result fraction from 42% to 0%. The pipeline had been
// discarding nearly half the geometry the GPU had already produced; throughput
// and waste turned out to be one problem.
//
// IT WAS FLIPPED ONCE PREMATURELY AND REVERTED, AND THAT IS WORTH KNOWING.
// Batching alone drove allocFail to 77,290 (96.9 M quads) -- this item's own
// revert condition -- which was published as "allocFail=0" without ever being
// read. It looked like first-fit fragmentation: 16,903 free runs, largest run
// 68,326 against 4,904,120 on the unbatched control at equal total free.
//
// IT WAS NOT AN ALLOCATOR PROBLEM. 3.3x apply throughput drove transient
// residency to 80,716 chunks because voxel.Stream.MaxUnloadsPerFrame was still
// 24 -- a value sized for a ~260 chunks/s pipeline -- so the unload queue grew
// without bound and a chronically over-full pool fragmented. Raising the unload
// budget took allocFail to 0 and holes to 0. No defrag was needed.
//
// So this default depends on TWO other numbers being right for the throughput it
// unlocks: MaxUnloadsPerFrame, and the 64M pool / 81,920 table floor. If any of
// them is lowered, re-measure allocFail here before assuming this is still safe.
static int32 GVoxelPoolBatchPublish = 1;
static FAutoConsoleVariableRef CVarVoxelPoolBatchPublish(
	TEXT("voxel.Stream.PoolBatchPublish"),
	GVoxelPoolBatchPublish,
	TEXT("1 = accumulate pool adds/removes across a streaming tick and publish ONCE at the end of it, ")
	TEXT("instead of once per mutated chunk. DEFAULT 1 since 2026-07-27: took the pooled arm from ")
	TEXT("~260 chunks/s / 15,032 holes to ~795 / single digits, closing the streaming-perf P0. Most of ")
	TEXT("that is the stale-result fraction going 42% -> 0% as the result queue stopped backing up. ")
	TEXT("0 restores the per-chunk path as the A/B control. NOTE it depends on MaxUnloadsPerFrame and ")
	TEXT("the pool/table sizing being right for the throughput it unlocks -- see the source comment."),
	ECVF_Default);

// S1-2 (docs/speculative-generation-plan.md Wave S1, §2.2): let a removed
// chunk's Allocations slot be REUSED by the next add, instead of Allocations
// only ever growing.
//
// DEFAULT 0 = BYTE-IDENTICAL TO TODAY. Gates the PUSH side only (see
// FreeHandles): with this off, RemoveChunkInternal never pushes a handle, so
// the free list stays empty forever and AddChunk/AddChunkFromGpu always
// append, exactly as before this cvar existed.
//
// What it is worth, measured before it was built
// (docs/measurements/s0-apply-census-2026-07-27.txt, "NEW FINDING 1"):
// Allocations is append-only today, so BuildChunkRuns's Reserve()-and-walk
// tracks chunks EVER ADDED, not resident, and its cost is unbounded in session
// length. Over one 120s flight leg mean allocations walked climbed 28,042 ->
// 68,416 while live runs emitted FELL 28,042 -> 18,389 -- a 3.72x and rising
// ratio of wasted walk on every publication. GetFreeHandleCount() is logged
// alongside allocsEver on the "Voxel GPU pool:" line so this is visible as it
// works.
// DEFAULT 1 SINCE 2026-07-27, and it is an UNBOUNDEDNESS fix rather than a
// speed-up -- do not expect chunks/s from it, and do not remove it for failing
// to deliver any. Measured A/B over four alternated flight legs: allocsEver
// 155,241 -> 62,299 (-60%), throughput 575.2 -> 579.3 which is INSIDE the
// 0.8-0.9% noise floor, and BuildChunkRuns total ms down only 6.2%.
//
// Why it does not speed anything up, which is worth recording because the
// opposite was predicted: BuildChunkRuns' cost tracks the runs it EMITS, not
// the array it walks. At matched emit, cutting the walk 45% moved cost 2% --
// the sort over ~50k live runs dominates, not the walk over Allocations.
//
// What it does fix is real: without it Allocations grows with chunks EVER ADDED
// for the lifetime of the process, so BuildChunkRuns' walk is unbounded in
// SESSION LENGTH rather than world size. That does not show up in a 5-minute
// leg and would show up in a long play session.
static int32 GVoxelPoolRecycleHandles = 1;
static FAutoConsoleVariableRef CVarVoxelPoolRecycleHandles(
	TEXT("voxel.Stream.PoolRecycleHandles"),
	GVoxelPoolRecycleHandles,
	TEXT("1 = RemoveChunkInternal pushes its freed Allocations handle onto a LIFO free list, ")
	TEXT("and AddChunk/AddChunkFromGpu pop from it before appending a new one. Default 0 ")
	TEXT("(byte-identical to before this existed: the free list stays empty and every add ")
	TEXT("appends). Fixes BuildChunkRuns walking chunks-ever-added instead of chunks-resident -- ")
	TEXT("see NEW FINDING 1 in docs/measurements/s0-apply-census-2026-07-27.txt."),
	ECVF_Default);

// Draws every chunk in the pool with a single mesh batch.
namespace VoxelGpuPoolCull
{
	// HARD CEILING ON DRAW RANGES *WITHIN ONE MESH BATCH*, and it is a correctness
	// limit rather than a taste one. The renderer selects batch elements with
	// `(1ull << BatchElementIndex) & BatchElementMask` (MeshPassProcessor.inl:167,
	// against a mask of ~0ull for dynamic elements -- MeshDrawCommands.cpp:644),
	// which is undefined for any index >= 64. GPUCullMaxRanges used to default to
	// 256, so a sufficiently fragmented view was one shift away from undefined
	// behaviour that would have presented as random missing terrain.
	//
	// IT IS NO LONGER A CEILING ON THE CULL'S RANGE BUDGET. The mask is per
	// FMeshBatch and every batch indexes its own elements from 0, so
	// GetDynamicMeshElements emits ceil(NumRanges / 64) batches and no batch is
	// ever asked about bit 64. See kMaxRanges for why the budget had to grow past
	// this number, and why it could not before.
	static constexpr int32 kMaxBatchElements = 64;

	// THE CULL'S ACTUAL RANGE BUDGET, now that the element mask is a per-batch
	// limit rather than a per-gather one.
	//
	// WHY 64 WAS EXPENSIVE, MEASURED AT 13,190 CHUNKS. The range-cap merge below
	// is optimal for "fewest redrawn quads subject to at most K ranges", so what
	// it draws at K=64 is the provable floor for a 64-range budget -- not slack to
	// be tuned away. That floor was 2.06x the visible quads. K=256 removed 38% of
	// that over-draw; K=1024 removed 85% and made the CAMERA gather converge to
	// exactly the visible set.
	//
	// At the adopted 128 m / 4 km cascade the pool holds ~39,020 chunks / 35.2M
	// quads, where full convergence needs ~10-14k ranges -- so 1024 is not the end
	// of the curve, it is where the remaining over-draw stops dominating. Read the
	// `budget:` log line (GPUCullStatsPeriod) for drawn(K) at the live pose rather
	// than assuming these numbers carry to a bigger cascade.
	//
	// WHAT RAISING IT COSTS: one extra FMeshBatch and one extra draw command per
	// 64 ranges -- 16 batches per gather at 1024, against ~39,020 draws if the
	// pool were drawn per chunk. Two orders of magnitude cheaper than the thing
	// this whole design exists to avoid. The one non-obvious cost is that every
	// range still allocates a single-frame uniform buffer for its BaseQuad
	// (FVoxelQuadRangeParameters), so the per-gather count of those scales with
	// this and not with the batch count.
	static constexpr int32 kMaxRanges = 1024;

	// DEFAULT ON since 2026-07-27 (was 0 -- and docs/gpu-roadmap-remaining.md
	// claiming otherwise is what let three baseline legs at the adopted 128 m /
	// 4 km cascade run uncull unnoticed at 45 ms/frame). Measured at the pinned
	// settle, 39,020 chunks / 35.2 M quads: cull off 44.5 ms, cull on 35.7 ms
	// at the OLD 64-range budget with drawn still 79% of the pool from merge
	// over-draw -- which is what the kMaxRanges=1024 budget above now attacks.
	static int32 GEnabled = 1;
	static FAutoConsoleVariableRef CVarEnabled(
		TEXT("voxel.Stream.GPUCull"), GEnabled,
		TEXT("Frustum-cull the GPU pool per chunk and draw only the surviving pool ranges. 1 = on (default ")
		TEXT("since 2026-07-27; measured -8.8ms at the 4km-cascade settle even at the old 64-range budget). ")
		TEXT("0 is one draw over the whole pool, which is the pre-cull behaviour and pays for ")
		TEXT("every resident quad regardless of where the camera looks."),
		ECVF_RenderThreadSafe);

	// Merging tolerance, in quads. Two surviving ranges separated by a gap
	// smaller than this are drawn as one, which re-draws the gap. That is a good
	// trade well past the point it looks wasteful: a quad costs six vertex
	// invocations and no pixels if it is off-screen, while a draw costs a state
	// change and a command. Tuned by measurement, not taste -- raise it if the
	// range count is high, lower it if the drawn-quad count is.
	//
	// THIS USED TO DO NOTHING. It was declared, documented, and referenced only
	// from a comment; the merge ran exclusively off a threshold derived from
	// GPUCullMaxRanges, so tuning this knob silently changed nothing and any
	// "tuned merge gap" measurement would have been measuring the default. It is
	// now the FIRST merge pass, with the range-cap merge below it as the backstop
	// that guarantees the element limit.
	static int32 GMergeGapQuads = 4096;
	static FAutoConsoleVariableRef CVarMergeGap(
		TEXT("voxel.Stream.GPUCullMergeGap"), GMergeGapQuads,
		TEXT("Quads. Surviving pool ranges closer together than this are merged into one draw, re-drawing ")
		TEXT("the gap between them. Trades redrawn off-screen quads against draw count. 0 disables this ")
		TEXT("pass, leaving only the range-cap merge."),
		ECVF_RenderThreadSafe);

	// The draw-range budget the merge works down to. Clamped to kMaxRanges
	// wherever it is read -- see that constant.
	//
	// DEFAULTS TO THE CAP, DELIBERATELY. Leaving this at the old 64 while the
	// ceiling moved would keep the measured 2.06x merge over-draw and make the
	// multi-batch emit a no-op that costs a comparison -- the ceiling moved
	// BECAUSE 64 ranges cannot describe what is visible at 39k chunks. Set it back
	// to 64 to reproduce the pre-multi-batch draw shape exactly; that is the
	// control this default should be read against.
	static int32 GMaxRanges = kMaxRanges;
	static FAutoConsoleVariableRef CVarMaxRanges(
		TEXT("voxel.Stream.GPUCullMaxRanges"), GMaxRanges,
		TEXT("Draw-range budget for the cull. The merge always reaches it by construction. Clamped to 1024. ")
		TEXT("Ranges are split across ceil(N/64) mesh batches: the renderer's batch-element mask is a uint64, ")
		TEXT("so index >= 64 is undefined WITHIN ONE BATCH, which is the only place it is a limit."),
		ECVF_RenderThreadSafe);

	// The budget actually used, never above the cap.
	inline int32 GetMaxRanges() { return FMath::Clamp(GMaxRanges, 1, kMaxRanges); }

	// Control experiment: run the whole range/merge/multi-draw path but treat
	// EVERY chunk as visible. If the picture is correct with this on and wrong
	// with it off, the frustum test is selecting the wrong chunks; if it is wrong
	// both ways, the fault is in emitting many draw elements per batch. One cvar
	// separates two hypotheses that produce the identical symptom.
	static int32 GDebugAllVisible = 0;
	static FAutoConsoleVariableRef CVarDebugAllVisible(
		TEXT("voxel.Stream.GPUCullDebugAllVisible"), GDebugAllVisible,
		TEXT("1 = skip the frustum test and treat every chunk as visible, while still going through the "
		     "range merge and multi-element draw path. Isolates 'the cull picks the wrong chunks' from "
		     "'many draw elements per batch is broken'."),
		ECVF_RenderThreadSafe);

	// The control DebugAllVisible could not be. With every chunk visible the
	// surviving runs are contiguous, so the merge collapses them to ONE range and
	// the experiment degenerates into the single full draw it was meant to be
	// compared against. This one ignores the runs entirely and chops [0, NumQuads)
	// into N equal contiguous ranges that together cover the pool exactly once, so
	// N draw elements are exercised while the drawn SET is provably identical to
	// the full draw. Any difference in the picture is the multi-element draw path,
	// not the chunk selection.
	static int32 GDebugSplit = 0;
	static FAutoConsoleVariableRef CVarDebugSplit(
		TEXT("voxel.Stream.GPUCullDebugSplit"), GDebugSplit,
		TEXT("N>1: ignore the frustum and draw the whole pool as N equal contiguous ranges. Covers exactly "
		     "the same quads as the single full draw, so it isolates 'many draw elements per batch' from "
		     "'the cull picks the wrong chunks'. 0/1 = off."),
		ECVF_RenderThreadSafe);

	// Draws the chunks the frustum test REJECTED instead of the ones it accepted.
	// A one-bit answer to "is the frustum test picking the right chunks": if it is,
	// this renders (nearly) nothing. Merging is skipped here, so what reaches the
	// screen is exactly the rejected set and nothing else.
	static int32 GDebugInvert = 0;
	static FAutoConsoleVariableRef CVarDebugInvert(
		TEXT("voxel.Stream.GPUCullDebugInvert"), GDebugInvert,
		TEXT("1 = draw exactly the runs the frustum test rejected, unmerged. If the cull is correct this "
		     "renders almost nothing; terrain still on screen is geometry the cull is wrongly discarding."),
		ECVF_RenderThreadSafe);

	inline bool IsEnabled() { return GEnabled != 0; }

	// HOW MANY LEVELS OF THE CASCADE CAST DYNAMIC SHADOWS.
	//
	// Measured 2026-07-26 (Wave G / G0): the four shadow cascades collectively see
	// 90.6% of the pool, against 7.4% for the camera at the same pose, and shadow
	// gathers submit 92.6% of all quads. That is not merge waste -- it is geometry
	// genuinely inside some cascade's frustum, so it is a floor that survives even
	// perfect compaction. The cause is that every resident chunk out to 2 km casts
	// a dynamic shadow, including the coarse outer rings whose shadow subtends
	// almost no screen area.
	//
	// This caps that by LEVEL, which maps directly to distance under
	// kDefaultRingPresets: L0 0-64 m, L1 64-128, L2 128-256, L3 256-512,
	// L4 512-1024, L5 1024-2048.
	//
	// THE DEFAULT IS THE CONSERVATIVE END ON PURPOSE. 4 drops only level 5 --
	// terrain beyond 1 km -- worth roughly 1.5M quads a frame off an 11.85M floor,
	// which is comparable to the ENTIRE camera-view over-draw the pool's frustum
	// cull was built to remove. The more aggressive settings are worth
	// substantially more (~3.3M at 3, ~5.3M at 2) and they are a QUALITY
	// judgement, not a performance one: distant terrain stops casting and, more
	// noticeably, stops SELF-shadowing, so far relief flattens and reads bright.
	// No harness in this project can score that -- it needs eyes on a low-sun
	// scene, which is why it is a live cvar and why the aggressive values are on
	// the manual checklist rather than in this default.
	static int32 GShadowMaxLevel = 4;
	static FAutoConsoleVariableRef CVarShadowMaxLevel(
		TEXT("voxel.Stream.GPUShadowMaxLevel"), GShadowMaxLevel,
		TEXT("Highest cascade level whose pool chunks cast dynamic shadows. Levels above this are culled "
		     "from shadow gathers only; the camera still draws every level it can see. Default 4 (terrain "
		     "beyond ~1 km stops casting). Lower saves more and costs distant self-shadowing. 5+ = no cap."),
		ECVF_RenderThreadSafe);

	// A chunk's level, recovered from the chunk table. AddChunk stores
	// Scale = float(1 << Level) in .w for the vertex factory's mip scale, so the
	// level is already resident and needs no new per-chunk data.
	inline int32 LevelFromScale(float Scale)
	{
		return Scale >= 1.0f ? int32(FMath::FloorLog2(uint32(Scale))) : 0;
	}

	// THE GATHER CENSUS (Wave G / G0). How many quads this proxy actually asks
	// the GPU to draw per frame, split by whether the gather was the camera or a
	// shadow cascade.
	//
	// WHY A NEW COUNTER RATHER THAN READING THE CULL LOG, which prints exactly
	// these numbers already: THE CULL LOG HAS BEEN REPORTING ONE GATHER, AND
	// ALWAYS THE SAME ONE, IN EVERY LEG EVER RUN.
	//
	// It fires on `CullLogCounter.Increment() % 600 == 1`.
	// FThreadSafeCounter::Increment returns the POST-increment value
	// (ThreadSafeCounter.h:52-55), so the samples land at gather ordinals
	// 0, 600, 1200, .... GetDynamicMeshElements is called once per view AND once
	// per shadow cascade, so with G gathers per frame the sampled gather's index
	// within its own frame is (600k) mod G -- which is 0 for every k whenever G
	// divides 600. And 600 = 2^3 * 3 * 5^2, so every plausible G (1..6, 8, 10,
	// 12, ...) divides it.
	//
	// The evidence it was aliased was already on the record, and was read as a
	// virtue: across the five recorded sessions there are 112 `cull: runs=`
	// samples, every one of them `shadowGather=0`, and `visibleQuads=164534`
	// repeats IDENTICALLY TO THE DIGIT across many samples and both
	// straight-down legs. A rotating sample cannot do that. An aliased one must.
	//
	// This is not a tidiness fix. Wave A's cost model -- ~1.19 us per 1000 quads
	// drawn, cross-validated to 1.1% across two poses -- divided a WHOLE-FRAME
	// delta-p50 by a MAIN-VIEW-ONLY delta-drawnQuads. If shadow gathers draw the
	// pool too then the true denominator is larger, the true slope is smaller,
	// and the saving compaction can recover is smaller with it. The 1.1%
	// agreement does not rescue it: both poses were measured through the same
	// aliased instrument, so a shared bias cancels in the cross-validation and
	// survives into the estimate.
	//
	// So this counter does not sample. It ACCUMULATES every gather, at the point
	// of submission, on the cull and uncull branches alike -- what it reports is
	// what the proxy asked the GPU for, not what one gather in six hundred did.
	static int32 GStatsPeriod = 0;
	static FAutoConsoleVariableRef CVarStatsPeriod(
		TEXT("voxel.Stream.GPUCullStatsPeriod"), GStatsPeriod,
		TEXT("Frames between gather-census reports. Counts every gather (camera and shadow) and reports "
		     "quads submitted per frame split by gather type. 0 = off (default). This is a COUNT, so it is "
		     "immune to the frame-time clamp and to GPU contention."),
		ECVF_RenderThreadSafe);
}

class FVoxelGpuPoolSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FVoxelGpuPoolSceneProxy(UVoxelGpuPoolComponent* Component,
	                        const TArray<uint64>& InQuads,
	                        const TArray<uint32>& InChunkIds,
	                        const TArray<FVector4f>& InOrigins,
	                        const TArray<FVector4f>& InParams,
	                        const TArray<UVoxelGpuPoolComponent::FChunkRun>& InRuns,
	                        const FString& InPoolName,
	                        int32 InChunkTableCapacity,
	                        FVoxelGpuPoolBuffersRef InSharedBuffers)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, PoolName(InPoolName)
		, SharedBuffers(MoveTemp(InSharedBuffers))
		, Quads(InQuads)
		, ChunkIds(InChunkIds)
		, Origins(InOrigins)
		, Params(InParams)
		, Runs(InRuns)
		, ChunkEdgeVoxels(Component->GetChunkEdgeVoxels())
		, NumQuads(Component->GetHighWaterMarkQuads())
		, BufferQuads(InQuads.Num())
		, NumChunks(InOrigins.Num())
		, MaxChunks(FMath::Max(InOrigins.Num() * 4, InChunkTableCapacity))
	{
		UMaterialInterface* Material = Component->GetChunkMaterialOrDefault();
		MaterialProxy = Material->GetRenderProxy();
		MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetFeatureLevel());

		#if !(UE_BUILD_SHIPPING)
		{
			TArray<UMaterialInterface*> UsedForVerification;
			UsedForVerification.Add(Material);
			SetUsedMaterialForVerification(UsedForVerification);
		}
		#endif
	}

	virtual ~FVoxelGpuPoolSceneProxy()
	{
		VertexFactory.ReleaseResource();
	}

	void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
	{
		// FIRST, AND NOT IN THE CONSTRUCTOR. The bounds are world-space, so they
		// need GetLocalToWorld() -- and FPrimitiveSceneProxy::LocalToWorld is a
		// private member with no default initialiser that is only ever assigned by
		// SetTransform (PrimitiveSceneProxy.cpp:907), so in the constructor it is
		// uninitialised memory. FScene::AddPrimitive enqueues SetTransform and this
		// method as ONE render command in that order, explicitly so that resources
		// may depend on the transform (RendererScene.cpp:1506-1514), which makes
		// this the earliest point the bounds can be built at all.
		//
		// Above the early return below: a proxy holding runs but no quads still
		// gets gathered, and the cull would otherwise run its whole first life on
		// the uncached fallback.
		RebuildRunBounds();

		if (BufferQuads == 0 || NumChunks == 0)
		{
			return;
		}

		// Static, NOT Dynamic -- this is load-bearing, and it is the opposite of
		// what "we update this every frame" instinctively suggests.
		//
		// These buffers are written in sub-ranges (see UpdateQuadRange_RenderThread).
		// In the D3D12 RHI only the *static* lock path honours a lock offset: it
		// allocates a staging buffer of exactly the locked size and issues a
		// CopyBufferRegion into the destination at that offset
		// (D3D12Buffer.cpp:750, :801, :818). The *dynamic* path ignores the offset
		// completely and hands back the buffer's base address (:659), then renames
		// the whole buffer to a fresh upload allocation on every lock after the
		// first (:667, :697) -- so everything outside the range just written
		// becomes uninitialised garbage. Marking these Dynamic silently corrupted
		// the chunk table and put every partial write at quad 0.
		// --- the persistent half (Wave D / D1) ----------------------------
		//
		// UPLOADED ON FIRST CREATION ONLY. This one branch is what removes the
		// CPU-shadow clobber: every later proxy rebinds buffers that already
		// hold the live geometry instead of overwriting them with whatever the
		// CPU shadow happens to contain. Once the GPU writes quads directly,
		// re-uploading here would silently revert them.
		check(SharedBuffers.IsValid());
		if (!SharedBuffers->IsValid() || SharedBuffers->CapacityQuads < Quads.Num())
		{
			// First proxy, or the pool outgrew the allocation. The latter means
			// the GPU-resident contents are genuinely gone, so re-uploading from
			// the CPU shadow is the correct and only thing to do.

			// REQUIREMENT D4-R1, made audible. A rebuild (as opposed to a first
			// creation) destroys the buffer every GPU-written range lives in, and
			// the CPU shadow holds zeros for those ranges -- so they come back
			// EMPTY, and nothing here re-dispatches them. Capacity is allocated
			// once from kPoolCapacityQuads and never grows in practice, so this
			// should be unreachable; "rare and silent" is the combination this
			// wave has repeatedly been caught by, so it is loud instead.
			if (SharedBuffers->CapacityQuads > 0)
			{
				UE_LOG(LogTemp, Error,
				       TEXT("%s: pool buffers REBUILT (capacity %d -> %d quads). Every GPU-written range is "
				            "now empty and nothing re-meshes them — see requirement D4-R1. Expect missing "
				            "terrain until those chunks are re-dispatched."),
				       *PoolName, SharedBuffers->CapacityQuads, Quads.Num());
			}
			SharedBuffers->GpuWritable.Set(0);
			SharedBuffers->QuadBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
				RHICmdList, TEXT("VoxelGpuPool.Quads"),
				EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource
					| EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer,
				MakeConstArrayView(Quads));
			SharedBuffers->QuadSRV = RHICmdList.CreateShaderResourceView(
				SharedBuffers->QuadBuffer,
				FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(SharedBuffers->QuadBuffer));
			SharedBuffers->QuadUAV = RHICmdList.CreateUnorderedAccessView(
				SharedBuffers->QuadBuffer,
				FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(SharedBuffers->QuadBuffer));

			SharedBuffers->ChunkIdBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
				RHICmdList, TEXT("VoxelGpuPool.ChunkIds"),
				EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource
					| EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer,
				MakeConstArrayView(ChunkIds));
			SharedBuffers->ChunkIdSRV = RHICmdList.CreateShaderResourceView(
				SharedBuffers->ChunkIdBuffer,
				FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(SharedBuffers->ChunkIdBuffer));
			SharedBuffers->ChunkIdUAV = RHICmdList.CreateUnorderedAccessView(
				SharedBuffers->ChunkIdBuffer,
				FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(SharedBuffers->ChunkIdBuffer));

			SharedBuffers->CapacityQuads = Quads.Num();

			// The RDG half (Wave D / D1). Same two buffers, wrapped so a compute
			// pass can write them: RegisterExternalBuffer brings them into a
			// graph, RDG emits the UAVCompute transition, and the default
			// epilogue access (SRVMask) hands them back in the state the draw
			// wants. No manual barrier anywhere, and the SRVs the vertex factory
			// baked into its uniform buffer keep working untouched -- a
			// transition applies to the RESOURCE, not the view.
			//
			// The descs must match how the buffers were actually created, above.
			// CreateStructuredDesc's default usage is
			// Static|UnorderedAccess|ShaderResource|StructuredBuffer, which is
			// exactly the flag set used there.
			SharedBuffers->QuadPooled = new FRDGPooledBuffer(
				RHICmdList, SharedBuffers->QuadBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint64), uint32(SharedBuffers->CapacityQuads)),
				uint32(SharedBuffers->CapacityQuads), TEXT("VoxelGpuPool.Quads"));
			SharedBuffers->ChunkIdPooled = new FRDGPooledBuffer(
				RHICmdList, SharedBuffers->ChunkIdBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), uint32(SharedBuffers->CapacityQuads)),
				uint32(SharedBuffers->CapacityQuads), TEXT("VoxelGpuPool.ChunkIds"));

			// LAST, after everything a direct write needs exists. The game thread
			// reads this to decide whether to dispatch a mesh job direct-to-pool,
			// and the only ordering that matters is that it cannot see 1 before
			// the buffers are there.
			SharedBuffers->GpuWritable.Set(1);

			UE_LOG(LogTemp, Log,
			       TEXT("%s: created persistent pool buffers, capacity %d quads — GPU-writable"),
			       *PoolName, SharedBuffers->CapacityQuads);
		}
		else
		{
			// The line that makes the clobber impossible rather than unlikely.
			UE_LOG(LogTemp, Log,
			       TEXT("%s: REBOUND persistent pool buffers (capacity %d quads) — no re-upload, "
			            "GPU-written quads preserved across proxy recreation"),
			       *PoolName, SharedBuffers->CapacityQuads);
		}

		QuadBuffer = SharedBuffers->QuadBuffer;
		QuadBufferSRV = SharedBuffers->QuadSRV;
		ChunkIdBuffer = SharedBuffers->ChunkIdBuffer;
		ChunkIdSRV = SharedBuffers->ChunkIdSRV;

		TArray<FVector4f> PaddedOrigins = Origins;
		PaddedOrigins.SetNumZeroed(MaxChunks);
		OriginBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkOrigins"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(PaddedOrigins));
		OriginSRV = RHICmdList.CreateShaderResourceView(
			OriginBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(OriginBuffer));

		TArray<FVector4f> PaddedParams = Params;
		PaddedParams.SetNumZeroed(MaxChunks);
		ParamsBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkParams"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(PaddedParams));
		ParamsSRV = RHICmdList.CreateShaderResourceView(
			ParamsBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParamsBuffer));

		VertexFactory.SetQuadBufferSRV(QuadBufferSRV);
		VertexFactory.SetPoolBuffers(OriginSRV, ChunkIdSRV, ParamsSRV);
		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogTemp, Log,
		       TEXT("%s: %d chunks, %d quads, %d triangles â€” ONE primitive, ONE draw"),
		       *PoolName, NumChunks, NumQuads, NumQuads * 2);

	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bDynamicRelevance = true;
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		// Matches FWaterChunkSceneProxy. Inert for an opaque material, so this
		// costs terrain nothing; it is what a TRANSLUCENT pool (the water
		// instance) needs in order to receive translucent self-shadowing the
		// same way the per-brick water components do.
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		// Must come after SetPrimitiveViewRelevance, which is what fills in
		// bOpaque. Same expression as FVoxelChunkSceneProxy so the two renderers
		// contribute to the velocity pass identically -- the factory already
		// implements VertexFactoryGetPreviousWorldPosition, so the geometry for
		// it was always there; only the relevance flag that asks for it was
		// missing, and without it TSR reprojects this terrain from depth alone.
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	                            const FSceneViewFamily& ViewFamily,
	                            uint32 VisibilityMap,
	                            FMeshElementCollector& Collector) const override
	{
		if (NumQuads == 0 || !QuadBufferSRV.IsValid() || MaterialProxy == nullptr)
		{
			// Silence is ambiguous: a pool that draws nothing looks identical
			// whether the renderer never asked or the batch was rejected. Say
			// which.
			if (ElementsLogged.Increment() == 1)
			{
				UE_LOG(LogTemp, Warning,
				       TEXT("%s draw SKIPPED: numQuads=%d srv=%d materialProxy=%d"),
				       *PoolName, NumQuads, QuadBufferSRV.IsValid() ? 1 : 0, MaterialProxy != nullptr ? 1 : 0);
			}
			return;
		}

		if (ElementsLogged.Increment() == 1)
		{
			// The budget and the per-batch element limit are printed because a
			// gather no longer submits exactly one FMeshBatch: with the cull on it
			// submits ceil(ranges / elementsPerBatch) of them, and a reader
			// counting draw commands against this line needs to know that before
			// concluding the pool is drawing more than it should.
			UE_LOG(LogTemp, Log,
			       TEXT("%s draw SUBMITTED: numQuads=%d views=%d visMap=0x%x cull=%d maxRanges=%d "
			            "elementsPerBatch=%d"),
			       *PoolName, NumQuads, Views.Num(), VisibilityMap,
			       VoxelGpuPoolCull::IsEnabled() ? 1 : 0, VoxelGpuPoolCull::GetMaxRanges(),
			       VoxelGpuPoolCull::kMaxBatchElements);
		}

		// Editor Wireframe view mode, mirroring FVoxelChunkSceneProxy. Without
		// this the pooled terrain is the one thing in the level that stays solid
		// when you switch to Wireframe -- which reads as "the pool is drawing
		// with the wrong material" rather than "wireframe is unimplemented here".
		// Same blue as the component path, so the two are indistinguishable when
		// voxel.Stream.GPUMaxLevel puts both renderers in one frame.
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
		const FMaterialRenderProxy* BatchMaterialProxy = MaterialProxy;
		if (bWireframe)
		{
			auto* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.5f, 1.f));
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
			BatchMaterialProxy = WireframeMaterialInstance;
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			// ONE element covering EVERY chunk, unless culling is on.
			//
			// One draw over the whole pool is the design, and it is what removes
			// the per-chunk cost from the renderer. What it also removes is
			// per-chunk FRUSTUM CULLING, and that is not free: measured on a
			// settled scene, the pooled path's frame time does not depend on what
			// is on screen at all (18.58 -> 19.05 ms when the camera is pointed at
			// almost nothing) while the per-chunk component path gets 64% cheaper
			// over the same change. A renderer whose cost is invariant to
			// visibility is a renderer that is not culling.
			//
			// So: cull per chunk on the render thread, then draw the surviving
			// pool RANGES. Chunks own contiguous spans, and the pool fills in
			// roughly spatial order, so neighbours in the pool are usually
			// neighbours in the world and the surviving spans merge back down to
			// far fewer draws than there are chunks. This keeps one primitive --
			// nothing here touches FScene -- and trades "one draw" for "one draw
			// per visible run", which is the trade the measurement says is worth
			// making.
			// LOCAL, not a member. GetDynamicMeshElements is called CONCURRENTLY --
			// bSupportsParallelGDME defaults to true, so the renderer gathers the
			// main view and every shadow cascade on parallel tasks, all through this
			// one const method on this one proxy. Holding the cull's working set in
			// mutable members therefore had several views writing the same array at
			// once, and what got drawn was a mixture of their answers: a plausible
			// count of quads, spatially unrelated to any one view's frustum. That is
			// the whole under-selection. It also crashed -- reassigning the array
			// while another thread iterated it asserted in the D3D12 RHI.
			TArray<FQuadRange> Ranges;
			// Threaded out of the cull rather than recomputed, so the census
			// reports the same number the cull acted on. Stays 0 on the uncull
			// control, where nothing measures visibility at all -- which the
			// census report must not present as "zero quads were visible".
			uint32 VisibleQuadsThisGather = 0;
			ECullOutcome Outcome = ECullOutcome::Ranges;
			const bool bCull = VoxelGpuPoolCull::IsEnabled() && Runs.Num() > 0 && NumQuads > 0;
			if (bCull)
			{
				BuildCulledRanges(*Views[ViewIndex], Ranges, VisibleQuadsThisGather, Outcome);
			}

			// EVERYTHING THIS GATHER COULD SEE IS ABOVE THE SHADOW CAP. Draw
			// nothing and submit nothing -- do NOT fall through to the full-pool
			// draw below, which is the conservative answer to a DIFFERENT question
			// (see ECullOutcome). The census still records the gather, at zero
			// quads, so a capped-out cascade is visible in the counts as a real
			// measured zero rather than as a gather that never happened.
			if (bCull && Outcome == ECullOutcome::AllCapped)
			{
				// Explicitly ZERO. Nothing is submitted on this path, and NO batch
				// is allocated at all -- so nothing can be asked how much work it
				// represents. See RecordGather for what an abandoned, recycled
				// FMeshBatch reported back when this path did allocate one.
				RecordGather(*Views[ViewIndex], /*SubmittedQuads=*/0, VisibleQuadsThisGather);
				continue;
			}

			// Per view, not per batch: the cull path emits several batches and
			// every one of them must answer this question identically.
			const bool bBatchCastShadow = IsShadowCast(Views[ViewIndex]);

			if (!bCull || Ranges.Num() == 0)
			{
				// THE FALLBACK IS DELIBERATELY STILL ONE BATCH WITH ONE ELEMENT,
				// byte-identical to what it was before multi-batching existed. It
				// is the control every cull measurement is read against, so it must
				// not acquire a batch-splitting loop it can never take (one range
				// can never exceed the 64-element limit).
				FMeshBatch& Mesh = Collector.AllocateMesh();
				InitMeshBatch(Mesh, BatchMaterialProxy, bWireframe, bBatchCastShadow);

				// Not culling, or the cull produced nothing usable -- draw
				// everything, which is always correct and never worse than
				// before this option existed.
				//
				// UserData stays null, so the factory binds its BaseQuad = 0
				// buffer and the shader computes QuadIndex = 0 + VertexId/6 --
				// the identical expression to before BaseQuad existed. This path
				// is unchanged, not merely equivalent, and that is deliberate:
				// it is the control every cull measurement is read against.
				//
				// RESET-AND-ADD RATHER THAN Elements[0], which is the one line of
				// this branch that is not what it was. AllocateMesh does not
				// construct: TChunkedArray::Add(1) (ChunkedArray.h:254) only bumps
				// a count, so the slot holds whatever the last gather left in it --
				// which, now that a cull gather populates up to 64 elements per
				// batch and 16 batches per gather, is very often a fully populated
				// element list whose UserData points at LAST frame's one-frame
				// resources. Writing Elements[0] and submitting left the other 63
				// in place. This makes the element list exactly one default element
				// (which is what a freshly constructed FMeshBatch carries,
				// MeshBatch.h:556) before it is filled, so the draw shape is the
				// one this path has always meant: one element, whole pool.
				Mesh.Elements.Reset();
				FMeshBatchElement& Element = Mesh.Elements.AddDefaulted_GetRef();
				Element.IndexBuffer = nullptr;
				Element.FirstIndex = 0;
				// Up to the high-water mark only: everything above it has never
				// been written, so there is nothing there to draw.
				Element.NumPrimitives = uint32(NumQuads) * 2;
				Element.MinVertexIndex = 0;
				Element.MaxVertexIndex = uint32(NumQuads) * 6 - 1;
				Element.NumInstances = 1;
				Element.PrimitiveUniformBuffer = GetUniformBuffer();

				// AFTER the elements are built and BEFORE they are handed over, so
				// what is counted is exactly what is submitted -- including both
				// fallback paths, which is the half the cull's own log cannot see.
				RecordGather(*Views[ViewIndex], CountSubmittedQuads(Mesh), VisibleQuadsThisGather);

				Collector.AddMesh(ViewIndex, Mesh);
			}
			else
			{
				// EVERY RANGE DRAWS FROM VERTEX 0 AND NAMES ITS START EXPLICITLY.
				//
				// The obvious encoding -- FirstIndex = Range.First * 6, and let
				// SV_VertexID carry the offset into the factory's
				// QuadIndex = VertexId/6 -- is what shipped, and it is wrong.
				// SV_VertexID does not include the draw's base vertex on D3D12;
				// RHISupportsAbsoluteVertexID (DataDrivenShaderPlatformInfo.h)
				// returns true only for Vulkan, and FLocalVertexFactory threads
				// the base through a uniform buffer for exactly this reason.
				//
				// Measured with the frustum test bypassed and the pool tiled into
				// N exact contiguous ranges (GPUCullDebugSplit): at N = 2, 8 and
				// 64 only the first 1/N of the pool reached the screen, while the
				// frame cost stayed flat and then rose (12.26 -> 11.86 -> 14.07 ms
				// p50) -- every range drew its full quad count starting at pool
				// quad 0, and N=64 simply added 63 draw calls on top. That is a
				// renderer silently drawing the wrong geometry, which is the
				// failure mode this pool is worst at showing.
				//
				// So: FirstIndex = 0 everywhere, and the start goes through
				// FVoxelQuadRangeParameters. Correct whether or not the platform
				// includes the base vertex, because VertexId now unambiguously
				// runs [0, Count*6) for every range.
				//
				// SEVERAL BATCHES, NOT ONE, AND THAT IS WHAT LETS THE BUDGET EXCEED
				// 64 (Wave G). The element mask is per FMeshBatch and every batch
				// indexes its own elements from 0, so ceil(R / 64) batches carry R
				// ranges with no index ever reaching bit 64. See
				// VoxelGpuPoolCull::kMaxRanges for the measurement that says the
				// old 64-range budget was drawing 2.06x the visible quads at 13,190
				// chunks, and what each extra batch costs.
				const int32 NumBatches =
					FMath::DivideAndRoundUp(Ranges.Num(), VoxelGpuPoolCull::kMaxBatchElements);
				// Summed across the batches and reported as ONE gather: the census
				// counts gathers, not draws, and a gather that suddenly reported
				// itself 16 times would divide every per-gather figure by 16.
				uint64 SubmittedQuadsThisGather = 0;

				for (int32 BatchIndex = 0; BatchIndex < NumBatches; ++BatchIndex)
				{
					const int32 FirstRange = BatchIndex * VoxelGpuPoolCull::kMaxBatchElements;
					const int32 EndRange = FMath::Min(FirstRange + VoxelGpuPoolCull::kMaxBatchElements,
					                                  Ranges.Num());

					FMeshBatch& Mesh = Collector.AllocateMesh();
					// Through the shared helper rather than a second copy of the
					// setup: every batch of a gather must carry the identical vertex
					// factory, material, type and shadow flag, and two copies of
					// that is how one of them quietly stops matching.
					InitMeshBatch(Mesh, BatchMaterialProxy, bWireframe, bBatchCastShadow);

					// Reset FIRST. A batch arrives carrying one default element
					// (MeshBatch.h:556) and, on a recycled slot, whatever the last
					// gather left in it -- see RecordGather for what reading a
					// recycled element reported. Reset makes the element count
					// exactly the number of ranges added below, which is what the
					// mask ~0ull is then intersected against.
					Mesh.Elements.Reset();
					Mesh.Elements.Reserve(EndRange - FirstRange);

					for (int32 RangeIndex = FirstRange; RangeIndex < EndRange; ++RangeIndex)
					{
						const FQuadRange& Range = Ranges[RangeIndex];

						// ONE FRAME RESOURCE, not a local and not a proxy member.
						// GetDynamicMeshElements only gathers; the bindings are read
						// later when the draw commands are built, by which time a
						// stack local is gone. The collector owns this until the
						// frame ends.
						FVoxelQuadRangeUserData& RangeData =
							Collector.AllocateOneFrameResource<FVoxelQuadRangeUserData>();
						FVoxelQuadRangeParameters RangeParams;
						RangeParams.BaseQuad = Range.First;
						RangeData.RangeUniformBuffer =
							TUniformBufferRef<FVoxelQuadRangeParameters>::CreateUniformBufferImmediate(
								RangeParams, UniformBuffer_SingleFrame);

						// PER-BATCH ELEMENT INDEXING, STARTING AT 0. Nothing here
						// carries the global range index: the element's position in
						// THIS batch is what the mask bit refers to, and its start
						// in the pool travels in the uniform buffer above. That
						// separation is the whole reason splitting is free.
						FMeshBatchElement& Element = Mesh.Elements.AddDefaulted_GetRef();
						Element.IndexBuffer = nullptr;
						Element.FirstIndex = 0;
						Element.NumPrimitives = Range.Count * 2;
						Element.MinVertexIndex = 0;
						Element.MaxVertexIndex = Range.Count * 6 - 1;
						Element.NumInstances = 1;
						Element.PrimitiveUniformBuffer = GetUniformBuffer();
						Element.UserData = &RangeData;
					}

					// Counted BEFORE the batch is handed over, per batch, so the
					// gather total is the sum of what was actually submitted rather
					// than a re-derivation from the range list.
					SubmittedQuadsThisGather += CountSubmittedQuads(Mesh);
					Collector.AddMesh(ViewIndex, Mesh);
				}

				// ONCE PER PROXY. The draw-command count per gather stopped being 1
				// and this is the line that says so; without it a reader seeing 16
				// commands where the design promises "ONE draw" has no way to tell
				// the split from a regression.
				if (BatchesLogged.Increment() == 1)
				{
					UE_LOG(LogTemp, Log,
					       TEXT("%s draw MULTI-BATCH: ranges=%d -> batches=%d (<=%d elements each), "
					            "maxRanges=%d -- one primitive still, ceil(ranges/%d) draw commands"),
					       *PoolName, Ranges.Num(), NumBatches, VoxelGpuPoolCull::kMaxBatchElements,
					       VoxelGpuPoolCull::GetMaxRanges(), VoxelGpuPoolCull::kMaxBatchElements);
				}

				RecordGather(*Views[ViewIndex], SubmittedQuadsThisGather, VisibleQuadsThisGather);
			}
		}
	}

	// One mesh batch's shared setup, factored out because a cull gather now emits
	// SEVERAL batches and the fallback emits one -- and every one of them has to
	// carry identical state. Duplicating seven assignments is how two batches of
	// the same gather end up disagreeing about, say, CastShadow.
	void InitMeshBatch(FMeshBatch& Mesh, const FMaterialRenderProxy* InMaterialProxy,
	                   bool bInWireframe, bool bInCastShadow) const
	{
		Mesh.VertexFactory = &VertexFactory;
		Mesh.MaterialRenderProxy = InMaterialProxy;
		Mesh.bWireframe = bInWireframe;
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastShadow = bInCastShadow;
	}

	// --- incremental update, render thread -------------------------------
	//
	// Writes only the quads that changed. The alternative -- rebuilding the
	// whole buffer -- is 75 MB per change at cascade scale, which is what makes
	// streaming through a pool viable or not.
	// NewQuads/NewIds hold ONLY the dirty range, indexed from zero; First is
	// where it lands in the GPU buffer.
	// One lock/unlock pair per RUN. NewQuads/NewIds are the flattened payload;
	// each run says where its slice starts in them and where it lands in the
	// pool. Several exact runs, not one span covering the untouched geometry
	// between them -- see UVoxelGpuPoolComponent::DirtyQuadRanges.
	void UpdateQuadRange_RenderThread(FRHICommandListBase& RHICmdList,
	                                  const TArray<uint64>& NewQuads,
	                                  const TArray<uint32>& NewIds,
	                                  const TArray<FVoxelQuadUploadRun>& UploadRuns,
	                                  int32 NewNumQuads)
	{
		if (!QuadBuffer.IsValid() || UploadRuns.Num() == 0)
		{
			NumQuads = NewNumQuads;
			return;
		}

		for (const FVoxelQuadUploadRun& Run : UploadRuns)
		{
			if (Run.Count == 0)
			{
				continue;
			}

			const uint32 QuadSizeBytes = Run.Count * sizeof(uint64);
			if (void* Dst = RHICmdList.LockBuffer(QuadBuffer, Run.First * sizeof(uint64),
			                                      QuadSizeBytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, NewQuads.GetData() + Run.SrcOffset, QuadSizeBytes);
				RHICmdList.UnlockBuffer(QuadBuffer);
			}

			const uint32 IdSizeBytes = Run.Count * sizeof(uint32);
			if (void* Dst = RHICmdList.LockBuffer(ChunkIdBuffer, Run.First * sizeof(uint32),
			                                      IdSizeBytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, NewIds.GetData() + Run.SrcOffset, IdSizeBytes);
				RHICmdList.UnlockBuffer(ChunkIdBuffer);
			}
		}

		NumQuads = NewNumQuads;
	}

	// The chunk table is tiny (one float4 + one float2 per chunk), so it is
	// rewritten wholesale rather than tracked range by range.
	void UpdateChunkTable_RenderThread(FRHICommandListBase& RHICmdList,
	                                   const TArray<FVector4f>& NewOrigins,
	                                   const TArray<FVector4f>& NewParams,
	                                   const TArray<UVoxelGpuPoolComponent::FChunkRun>& NewRuns)
	{
		// Origins and Runs are the cull's two inputs and must not disagree: a run
		// naming a chunk id the table no longer describes would be culled against
		// a stale box. They are updated together, from the same game-thread
		// snapshot, for that reason.
		Origins = NewOrigins;
		// Assigned unconditionally. This is only ever called with a fresh
		// game-thread snapshot of both, so an EMPTY run list means the pool
		// genuinely holds no live chunks -- the previous "keep the old runs if the
		// new list is empty" guard turned that state into stale runs pointing at
		// quads that no longer belong to anyone.
		Runs = NewRuns;

		// THE ONE WRITER OF THE CULL'S PRECOMPUTED BOUNDS, and it is here rather
		// than anywhere else because Runs and Origins -- the only two inputs those
		// bounds are derived from -- are replaced wholesale on this line and the
		// one above it. No new invalidation logic exists or is needed: the caller's
		// existing bRunsDirty/bChunkTableDirty flags already gate every arrival of
		// a new run list, so anything that can change the bounds must come through
		// here. See RebuildRunBounds for the concurrency argument.
		//
		// BEFORE the early return below. That return means "the table outgrew its
		// GPU allocation, the caller will rebuild the render state" -- but Runs and
		// Origins have already been taken, the cull will read them on the next
		// gather, and it must not read them against bounds built from the previous
		// pair.
		RebuildRunBounds();

		if (!OriginBuffer.IsValid() || NewOrigins.Num() > MaxChunks)
		{
			return;   // outgrew the table; the caller rebuilds the render state
		}

		const uint32 OriginBytes = uint32(NewOrigins.Num()) * sizeof(FVector4f);
		if (void* Dst = RHICmdList.LockBuffer(OriginBuffer, 0, OriginBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewOrigins.GetData(), OriginBytes);
			RHICmdList.UnlockBuffer(OriginBuffer);
		}

		const uint32 ParamsBytes = uint32(NewParams.Num()) * sizeof(FVector4f);
		if (void* Dst = RHICmdList.LockBuffer(ParamsBuffer, 0, ParamsBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewParams.GetData(), ParamsBytes);
			RHICmdList.UnlockBuffer(ParamsBuffer);
		}
	}

	int32 GetMaxChunks() const { return MaxChunks; }

	// Same expression FVoxelChunkSceneProxy and FWaterChunkSceneProxy use. For
	// an opaque material this is the base class's answer anyway; it is spelled
	// out because the translucent (water) instance is the case where the two
	// answers could diverge.
	bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }

	uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	mutable FVoxelQuadVertexFactory VertexFactory;

	// Which pool this is, for the log lines below. There is more than one
	// instance now and they are diagnosed almost entirely from those lines.
	FString PoolName;

	// The component's buffers, not the proxy's. Held by shared pointer so this
	// proxy dying does not take the GPU-resident geometry with it — see
	// FVoxelGpuPoolBuffers for why that matters.
	FVoxelGpuPoolBuffersRef SharedBuffers;

	TArray<uint64> Quads;
	TArray<uint32> ChunkIds;
	TArray<FVector4f> Origins;
	TArray<FVector4f> Params;
	// Which quads belong to which chunk, sorted by pool offset. The cull's
	// input; see UVoxelGpuPoolComponent::FChunkRun for why the proxy cannot
	// derive this itself.
	TArray<UVoxelGpuPoolComponent::FChunkRun> Runs;
	int32 ChunkEdgeVoxels = 32;

	struct FQuadRange { uint32 First = 0; uint32 Count = 0; };

	// --- the cull's precomputed, view-independent half (Wave G) -------------
	//
	// EVERYTHING THE PER-RUN CULL BODY DID BEFORE IntersectBox, HOISTED OUT OF THE
	// PER-GATHER LOOP. That body used to do, per run per gather:
	// Origins[Run.ChunkId] (a random gather into a 39,020-entry array; chunk ids
	// are recycled LIFO and therefore uncorrelated with pool order, so it is a
	// near-guaranteed cache miss), LevelFromScale, an FBox construct, a
	// TransformBy(FMatrix) in LWC doubles, then GetCenter/GetExtent. Only the
	// IntersectBox that follows depends on the VIEW.
	//
	// WHAT THAT COST, at the adopted 128 m / 4 km cascade: ~39,020 runs tested per
	// gather across the camera and ~4 shadow gathers -- ~195,000 tests a frame at
	// 45-90 ns each, ~11.7 ms of render-thread CPU (~3-8 ms wall once the gathers
	// parallelise), of which 60-75% was everything above rather than the frustum
	// test itself.
	//
	// The SKIP REASONS are carried here rather than recomputed because they come
	// from the same two loads the gather was for: "is this a valid table index"
	// and "is its scale zero".
	enum class ERunBoundsState : uint8
	{
		Ok,       // drawable: Center/Extent/Level are meaningful
		BadId,    // the run names a chunk id the table does not describe
		Hidden,   // the table entry's scale is 0 -- collapsed to a point
	};

	// ~56 bytes per run, so ~2.2 MB at 39,020 runs. Read strictly sequentially,
	// in step with Runs, which is the entire point: one prefetchable stream
	// instead of 39,020 dependent random loads.
	//
	// DOUBLES, not floats. The world runs at ~8.4M UU from the origin so the
	// centre needs the range, and the extent is kept in the same precision so the
	// pair is bit-for-bit what FBox::TransformBy/GetCenter/GetExtent produced
	// before -- no razor-edge frustum result can flip on a rounding change, and
	// the census counts stay comparable across the change.
	struct FVoxelRunBounds
	{
		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		int32 Level = 0;
		ERunBoundsState State = ERunBoundsState::BadId;
	};

	// Parallel to Runs, same index. Written ONLY by RebuildRunBounds.
	TArray<FVoxelRunBounds> RunBounds;

	// The transform the bounds above were built with, and the flag that says they
	// were built at all.
	//
	// WHY THE TRANSFORM IS CHECKED RATHER THAN ASSUMED CONSTANT. The bounds are
	// world-space, so they are view-independent only for a FIXED LocalToWorld --
	// and this component's is not fixed forever. The pool is re-based once, by
	// SetWorldLocation AFTER RegisterComponent (FVoxelWorldImpl::GetOrCreateGpuPool
	// -- the chunk table is float32 and the world is 8.4M UU out, so the big
	// offset has to live in the component transform), which reaches the live proxy
	// as a SetTransform. FPrimitiveSceneProxy::ApplyWorldOffset is a second such
	// path. Either would leave these boxes describing where the chunks used to be,
	// and a frustum test on a stale box is the "a perfectly correct cull selects
	// the wrong geometry" failure this file has already been bitten by once.
	//
	// So the gather compares and falls back rather than trusting. One
	// FMatrix::Equals per gather, not per run.
	FMatrix RunBoundsLocalToWorld = FMatrix::Identity;
	bool bRunBoundsValid = false;

	// One Warning, ever, if a gather has to take the uncached path. That path is
	// exactly as fast as this code was before the hoist, so it is not a
	// correctness problem -- but a proxy stuck on it permanently (a transform that
	// changed, and a chunk table that then never updated again) silently gives
	// back the whole ~11.7 ms, which is precisely the "rare, silent, looks like it
	// works" shape the rest of this file is written against.
	mutable FThreadSafeCounter RunBoundsStaleLogged;

	// The view-independent half of the old per-run cull body, in ONE place so the
	// writer and the uncached fallback cannot drift apart -- two copies of this
	// arithmetic is how a cached box and a recomputed box end up disagreeing about
	// which chunks are on screen.
	FVoxelRunBounds ComputeRunBounds(const UVoxelGpuPoolComponent::FChunkRun& Run,
	                                 const FMatrix& LocalToWorldMatrix) const
	{
		FVoxelRunBounds Out;
		if (!Origins.IsValidIndex(int32(Run.ChunkId)))
		{
			Out.State = ERunBoundsState::BadId;
			return Out;
		}

		const FVector4f& Entry = Origins[int32(Run.ChunkId)];
		const float Scale = Entry.W;
		if (Scale <= 0.f)
		{
			Out.State = ERunBoundsState::Hidden;   // collapsed to a point, nothing to draw
			return Out;
		}

		// Already resident -- AddChunk stores float(1 << Level) here for the
		// vertex factory's mip scale, so the shadow cap needs no new per-chunk
		// data and neither does this.
		Out.Level = VoxelGpuPoolCull::LevelFromScale(Scale);

		// Same extent AddChunk grows LocalBounds by, so the cull can never be
		// tighter than the bounds the scene already culled the whole primitive
		// against.
		const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Scale;
		const FVector Min(Entry.X, Entry.Y, Entry.Z);
		const FBox LocalBox(Min, Min + FVector(EdgeUU));
		const FBox WorldBox = LocalBox.TransformBy(LocalToWorldMatrix);
		Out.Center = WorldBox.GetCenter();
		Out.Extent = WorldBox.GetExtent();
		Out.State = ERunBoundsState::Ok;
		return Out;
	}

	// THE SINGLE WRITER, AND THE CONST/CONCURRENCY CONTRACT IT SATISFIES.
	//
	// GetDynamicMeshElements is const and is called CONCURRENTLY -- once for the
	// camera and once per shadow cascade, on parallel tasks, through this one
	// proxy (see the note in that method about what happened when the cull's
	// working set was a member). Anything it reads must therefore be written only
	// OUTSIDE a gather.
	//
	// RunBounds / RunBoundsLocalToWorld / bRunBoundsValid are written only here,
	// and this runs only on the render thread from CreateRenderThreadResources and
	// UpdateChunkTable_RenderThread -- neither of which can overlap a gather. That
	// is exactly the contract Runs and Origins already live under, from the same
	// two call sites, which is why no new synchronisation appears with this array:
	// it is not a new kind of state, it is a precomputed function of state that
	// already had this property.
	void RebuildRunBounds()
	{
		// Wave S0 census. Cycles, not milliseconds: the conversion is a
		// floating-point multiply and this is the render thread's own path, so it
		// is done once at drain time on the game thread instead of once per call
		// here. Counters are unconditional (three atomic adds); the clock pair is
		// gated -- see GVoxelPoolPushStats.
		const bool bTime = GVoxelPoolPushStats != 0;
		const uint64 StartCycles = bTime ? FPlatformTime::Cycles64() : 0;

		RunBoundsLocalToWorld = GetLocalToWorld();
		RunBounds.Reset(Runs.Num());
		for (const UVoxelGpuPoolComponent::FChunkRun& Run : Runs)
		{
			RunBounds.Add(ComputeRunBounds(Run, RunBoundsLocalToWorld));
		}
		bRunBoundsValid = true;

		if (SharedBuffers.IsValid())
		{
			SharedBuffers->RunBoundsCalls.Add(1);
			SharedBuffers->RunBoundsRunsWalked.Add(int64(Runs.Num()));
			if (bTime)
			{
				SharedBuffers->RunBoundsCycles.Add(int64(FPlatformTime::Cycles64() - StartCycles));
			}
		}
	}

	// WHY AN EMPTY RANGE LIST IS NOT ONE SITUATION.
	//
	// Before the shadow cap existed, "no ranges" had exactly one meaning -- the
	// cull selected nothing -- and exactly one safe response: draw the whole pool.
	// A cull that wrongly hides the world is the failure this pool is worst at
	// diagnosing, so the conservative branch is the one that draws.
	//
	// THE SHADOW CAP INVERTS THAT, and it is the single most dangerous thing about
	// building it. A distant cascade whose entire visible set sits above
	// GPUShadowMaxLevel legitimately has nothing to draw. Routed through the old
	// fallback it would draw 13.09M quads instead of 0 -- turning the largest
	// saving on the board into the largest regression on the board, silently, and
	// looking exactly like the feature working.
	//
	// So the empty case carries WHICH empty it is, and the two are handled
	// oppositely. Logged when it fires, because neither is visible in a picture.
	enum class ECullOutcome : uint8
	{
		Ranges,          // normal: draw what came back
		NothingVisible,  // nothing passed the frustum -> draw everything (conservative, unchanged)
		AllCapped,       // everything visible was above the shadow cap -> draw NOTHING (correct)
	};

	// Frustum-test every run, then merge the survivors back into as few pool
	// ranges as possible. Runs arrive sorted by pool offset, so the merge is a
	// single linear pass.
	//
	// RE-ENTRANT BY CONSTRUCTION. Everything this touches is either read-only
	// proxy state or a local -- see the note in GetDynamicMeshElements about
	// bSupportsParallelGDME. The earlier version kept the working arrays as
	// mutable members "reused across frames so the per-frame cull allocates
	// nothing", which is a sound instinct for a method called once per frame and
	// wrong for one called once per frame PER VIEW, in parallel.
	void BuildCulledRanges(const FSceneView& View, TArray<FQuadRange>& CulledRanges,
	                       uint32& OutVisibleQuads, ECullOutcome& OutOutcome) const
	{
		const FMatrix& LocalToWorldMatrix = GetLocalToWorld();
		uint32 VisibleQuads = 0;
		OutOutcome = ECullOutcome::Ranges;
		// Assigned on every return path below via this reference, so a caller
		// cannot read a stale or uninitialised count from an early exit.
		ON_SCOPE_EXIT { OutVisibleQuads = VisibleQuads; };

		// N EQUAL RANGES OVER THE WHOLE POOL, no frustum test. See GDebugSplit.
		// Capped at the same budget GetMaxRanges() is, and for the same reason it
		// is no longer 64: the parts are split across ceil(N/64) mesh batches, so
		// the uint64 element mask is never indexed past bit 63 whatever N is. Below
		// 64 this path is unchanged, which matters because its published results
		// (N = 2, 8, 64) are what proved the FirstIndex encoding wrong.
		const int32 SplitCount = FMath::Min(VoxelGpuPoolCull::GDebugSplit,
		                                    VoxelGpuPoolCull::kMaxRanges);
		if (SplitCount > 1 && NumQuads > 0)
		{
			const uint32 Total = uint32(NumQuads);
			const uint32 Parts = uint32(FMath::Min(SplitCount, NumQuads));
			for (uint32 I = 0; I < Parts; ++I)
			{
				// Computed from the boundaries rather than a running total so the
				// parts tile [0, Total) exactly, with no rounding gap or overlap.
				const uint32 Begin = uint32((uint64(Total) * I) / Parts);
				const uint32 End = uint32((uint64(Total) * (I + 1)) / Parts);
				if (End > Begin)
				{
					CulledRanges.Add(FQuadRange{ Begin, End - Begin });
				}
			}
			// Deliberately NOT merged: adjacent ranges have a zero gap, so the
			// merge below would collapse them straight back to one draw and the
			// experiment would test nothing.
			// Prime, for the reason spelled out at the main cull log below.
			if (CullLogCounter.Increment() % 601 == 1)
			{
				UE_LOG(LogTemp, Log, TEXT("%s cull: DEBUG SPLIT into %d ranges covering %d quads"),
				       *PoolName, CulledRanges.Num(), NumQuads);
			}
			return;
		}

		// WHICH FRUSTUM. GetDynamicMeshElements is called for shadow gathers as
		// well as for the camera (FProjectedShadowInfo::GatherDynamicMeshElements),
		// and there the view handed in is a SNAPSHOT of the main view -- so
		// View.ViewFrustum is still the camera's. Culling shadow casters against
		// the camera means every caster outside the camera frustum stops casting.
		// The renderer's own answer to this is a per-gather cull frustum on the
		// view, which is the shadow's bounds; its planes are expressed in
		// pre-shadow-translated world space, so the translation has to come back
		// out before they can be tested against absolute world boxes. Same
		// adjustment FHierarchicalStaticMeshSceneProxy makes for foliage.
		FConvexVolume ShadowFrustumLocal;
		const FConvexVolume* Frustum = &View.ViewFrustum;
		const bool bShadowGather = View.GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		if (bShadowGather)
		{
			const FConvexVolume& ShadowFrustum = *View.GetDynamicMeshElementsShadowCullFrustum();
			for (const FPlane& Src : ShadowFrustum.Planes)
			{
				FPlane Norm = Src / Src.Size();
				Norm.W -= (FVector(Norm) | View.GetPreShadowTranslation());
				ShadowFrustumLocal.Planes.Add(Norm);
			}
			ShadowFrustumLocal.Init();
			Frustum = &ShadowFrustumLocal;
		}

		// Why each run was dropped. Every one of these paths is a `continue` that
		// looks exactly like a frustum rejection from outside, and one of them
		// silently dropping most of the pool is indistinguishable in the picture
		// from a cull that aims wrongly.
		int32 SkippedEmpty = 0, SkippedBadId = 0, SkippedAboveWatermark = 0, SkippedHidden = 0, SkippedFrustum = 0;
		// Chunks that PASSED the frustum test and were then removed by the shadow
		// level cap. See the cap's own comment for why the ordering matters.
		int32 SkippedShadowLevel = 0;
		uint32 CappedQuads = 0;
		uint32 RunQuads = 0;

		// PER-LEVEL BREAKDOWN, to replace the one estimate in the shadow-cap
		// costing with a measurement.
		//
		// That costing priced each cap setting by assuming quads-per-chunk is
		// roughly uniform across levels -- plausible, because the clipmap holds
		// every chunk at 32^3 voxels whatever its level, and probably conservative,
		// because coarse terrain is smoother and should mesh to FEWER quads. But
		// "probably conservative" is how estimates have gone wrong in both
		// directions in this programme, and the pool knows the real answer.
		//
		// Pool[] is view-independent (every run, regardless of frustum) and is what
		// validates or refutes the uniformity assumption. Visible[] is per gather
		// and is what a cap would actually remove from THIS cascade.
		static constexpr int32 kMaxLevels = 8;
		uint32 PoolQuadsByLevel[kMaxLevels] = {};
		uint32 VisibleQuadsByLevel[kMaxLevels] = {};

		// THE HOIST, AND THE ONE CONDITION IT DEPENDS ON. See RunBounds for what
		// the per-run body used to do and what it cost. The bounds are world-space,
		// so they are only usable while the transform they were built with is still
		// the transform in force -- checked ONCE here rather than assumed, because
		// the pool is re-based after registration and ApplyWorldOffset exists.
		//
		// The uncached path is not a failure path, it is the OLD path: it recomputes
		// exactly what RebuildRunBounds would have, per run, with no allocation, so
		// the picture is identical either way and only the cost differs. It is
		// logged once so a proxy that never leaves it cannot do so silently.
		const bool bUseCachedBounds = bRunBoundsValid
		                              && RunBounds.Num() == Runs.Num()
		                              && RunBoundsLocalToWorld.Equals(LocalToWorldMatrix);
		if (!bUseCachedBounds && RunBoundsStaleLogged.Increment() == 1)
		{
			UE_LOG(LogTemp, Warning,
			       TEXT("%s cull: run bounds NOT CACHED (valid=%d bounds=%d runs=%d transformMatches=%d) -- "
			            "falling back to the per-run recompute, which is correct but is the whole pre-hoist "
			            "cost (~11.7 ms of render-thread CPU at cascade scale). Expect this to clear on the "
			            "next chunk-table update; if it does not, the transform changed and nothing has "
			            "pushed a table update since."),
			       *PoolName, bRunBoundsValid ? 1 : 0, RunBounds.Num(), Runs.Num(),
			       RunBoundsLocalToWorld.Equals(LocalToWorldMatrix) ? 1 : 0);
		}

		// Declared out of the loop so the cached path never touches it: on that
		// path this is dead storage and the reference below binds straight into
		// RunBounds.
		FVoxelRunBounds ScratchBounds;

		for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
		{
			const UVoxelGpuPoolComponent::FChunkRun& Run = Runs[RunIndex];
			// Named RunB, not Bounds: FPrimitiveSceneProxy::Bounds is a class
			// member and C4458 (declaration hides member) is warning-as-error.
			const FVoxelRunBounds& RunB =
				bUseCachedBounds ? RunBounds[RunIndex]
				                 : (ScratchBounds = ComputeRunBounds(Run, LocalToWorldMatrix));

			RunQuads += Run.NumQuads;
			if (Run.NumQuads == 0 || RunB.State == ERunBoundsState::BadId)
			{
				++(Run.NumQuads == 0 ? SkippedEmpty : SkippedBadId);
				continue;
			}
			// Ranges above the high-water mark hold nothing that was ever
			// written; drawing them would be reading uninitialised pool.
			//
			// STAYS IN THE LOOP, deliberately, while everything else moved out:
			// NumQuads is advanced by UpdateQuadRange_RenderThread, which runs
			// WITHOUT a table update whenever only quads changed, so this is the
			// one test here that is not a function of Runs and Origins alone.
			if (Run.FirstQuad + Run.NumQuads > uint32(NumQuads))
			{
				++SkippedAboveWatermark;
				continue;
			}

			if (RunB.State == ERunBoundsState::Hidden)
			{
				++SkippedHidden;
				continue; // hidden entry: collapsed to a point, nothing to draw
			}

			const int32 ChunkLevel = RunB.Level;
			const int32 LevelSlot = FMath::Clamp(ChunkLevel, 0, kMaxLevels - 1);
			PoolQuadsByLevel[LevelSlot] += Run.NumQuads;

			// One-shot: the actual numbers the frustum test is fed, against the
			// view it is tested with. A cull that selects the wrong chunks and a
			// cull that selects none look identical from the outside, and both
			// read as "the pool is broken"; this prints the operands instead of
			// inferring them.
			//
			// THE GetValue() SHORT-CIRCUIT IS NOT COSMETIC. Increment() alone is an
			// atomic read-modify-write, and it sat in the per-run body: ~39,020 of
			// them per gather, from ~5 gathers running in parallel, all on the one
			// cache line -- a contended RMW that costs more than the frustum test it
			// guards and scales with the cascade. The load in front of it is a
			// shared read of a clean line after the first gather, and the semantics
			// are unchanged: two threads that both read 0 still race to Increment
			// and exactly one of them gets 1.
			if (CullSpaceLogged.GetValue() == 0 && CullSpaceLogged.Increment() == 1)
			{
				// Re-read the table entry HERE rather than carrying it through the
				// loop. This block runs once in the life of the proxy, so the gather
				// it costs is free; keeping Entry/EdgeUU live for every run in order
				// to print them once is exactly the cost this hoist removed.
				const FVector4f& Entry = Origins[int32(Run.ChunkId)];
				const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Entry.W;
				const FBox WorldBox(RunB.Center - RunB.Extent, RunB.Center + RunB.Extent);
				UE_LOG(LogTemp, Warning,
				       TEXT("%s cullspace: chunk0 local=(%.0f,%.0f,%.0f) edge=%.0f -> worldCentre=(%.0f,%.0f,%.0f) ext=(%.0f,%.0f,%.0f) | viewOrigin=(%.0f,%.0f,%.0f) | l2wTrans=(%.0f,%.0f,%.0f)"),
				       *PoolName, Entry.X, Entry.Y, Entry.Z, EdgeUU,
				       WorldBox.GetCenter().X, WorldBox.GetCenter().Y, WorldBox.GetCenter().Z,
				       WorldBox.GetExtent().X, WorldBox.GetExtent().Y, WorldBox.GetExtent().Z,
				       View.ViewMatrices.GetViewOrigin().X, View.ViewMatrices.GetViewOrigin().Y,
				       View.ViewMatrices.GetViewOrigin().Z,
				       LocalToWorldMatrix.GetOrigin().X, LocalToWorldMatrix.GetOrigin().Y,
				       LocalToWorldMatrix.GetOrigin().Z);
			}

			// THE ONLY VIEW-DEPENDENT LINE IN THE BODY, which is the whole point of
			// the hoist. Centre and extent arrive already computed and already in
			// world space, so this is a handful of dot products against a
			// sequentially-streamed struct -- no gather, no matrix, no FBox.
			const bool bInFrustum = VoxelGpuPoolCull::GDebugAllVisible != 0 ||
			                        Frustum->IntersectBox(RunB.Center, RunB.Extent);
			const bool bKeep = VoxelGpuPoolCull::GDebugInvert != 0 ? !bInFrustum : bInFrustum;
			if (!bKeep)
			{
				++SkippedFrustum;
				continue;
			}

			// THE SHADOW-LEVEL CAP, AND IT IS DELIBERATELY *AFTER* THE FRUSTUM
			// TEST rather than before it.
			//
			// Testing first would be marginally cheaper and would make this
			// counter a lie: it would then count chunks the frustum was going to
			// reject anyway, and the empty-case decision below turns on exactly
			// this number meaning "geometry this cascade WOULD have drawn, which
			// only the cap removed". A counter that decides a branch has to mean
			// what the branch assumes it means. The frustum test is a handful of
			// dot products; correctness wins.
			//
			// Shadow gathers only. The camera must still draw every level it can
			// see, or the cap would delete visible terrain rather than its
			// shadow.
			// Counted BEFORE the cap, so it answers "what would this gather draw if
			// nothing were capped" -- which is the quantity a cap setting is chosen
			// against. Counting it after would make every level above the cap read
			// as zero and the breakdown would only ever confirm the cap already in
			// force.
			VisibleQuadsByLevel[LevelSlot] += Run.NumQuads;

			if (bShadowGather && ChunkLevel > VoxelGpuPoolCull::GShadowMaxLevel)
			{
				++SkippedShadowLevel;
				CappedQuads += Run.NumQuads;
				continue;
			}

			VisibleQuads += Run.NumQuads;
			CulledRanges.Add(FQuadRange{ Run.FirstQuad, Run.NumQuads });
		}

		// Inverted: what reaches the screen must be exactly the rejected set, so
		// merging (which only ever ADDS quads) would destroy the experiment.
		if (VoxelGpuPoolCull::GDebugInvert != 0)
		{
			// Prime, for the reason spelled out at the main cull log below.
			if (CullLogCounter.Increment() % 601 == 1)
			{
				UE_LOG(LogTemp, Log,
				       TEXT("%s cull: DEBUG INVERT drawing the REJECTED set: ranges=%d quads=%u/%d"),
				       *PoolName, CulledRanges.Num(), VisibleQuads, NumQuads);
			}
			return;
		}

		// THE RANGE-BUDGET CURVE. What would a bigger element budget buy?
		//
		// Computed from the UNMERGED survivors, before either merge pass, because
		// that is the only point at which the full gap structure still exists.
		//
		// The arithmetic is exact rather than a simulation. Merging a set of gaps
		// adds exactly those gaps to the drawn total, and PASS 2 below merges the
		// smallest gaps first -- which is optimal for "fewest redrawn quads subject
		// to at most K ranges". So for R surviving runs,
		//
		//     drawn(K) = visible + (sum of the R-K smallest gaps),  K < R
		//     drawn(K) = visible,                                   K >= R
		//
		// Two things follow, and both matter more than the numbers.
		//
		// FIRST: the drawn total this cull already achieves at K=64 IS drawn(64).
		// The over-draw at the cap is not slack in the implementation to be tuned
		// away -- it is the provable floor for a 64-range budget. Nothing that
		// keeps the range shape can beat it.
		//
		// SECOND: drawn(infinity) = visible is exactly what compaction delivers,
		// because a compacted id list has no range structure to merge. So this one
		// line prices the entire compaction argument against the cheap alternative
		// of simply spending more batch elements, per gather, from counts alone --
		// with no indirect draw, no view extension and no shader change built to
		// find out.
		if (VoxelGpuPoolCull::GStatsPeriod > 0 && CulledRanges.Num() > 1)
		{
			TArray<uint32> Gaps;
			Gaps.Reserve(CulledRanges.Num() - 1);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				const uint32 PrevEnd = CulledRanges[I - 1].First + CulledRanges[I - 1].Count;
				Gaps.Add(CulledRanges[I].First >= PrevEnd ? CulledRanges[I].First - PrevEnd : 0u);
			}
			Gaps.Sort();

			const int32 R = CulledRanges.Num();
			auto DrawnAt = [&Gaps, R, VisibleQuads](int32 K) -> uint64
			{
				if (K >= R)
				{
					return VisibleQuads;
				}
				uint64 Extra = 0;
				const int32 Merges = R - K;
				for (int32 I = 0; I < Merges && I < Gaps.Num(); ++I)
				{
					Extra += Gaps[I];
				}
				return uint64(VisibleQuads) + Extra;
			};

			if (CullLogCounter.GetValue() % 601 == 0)
			{
				UE_LOG(LogTemp, Log,
				       TEXT("%s budget: shadowGather=%d runs=%d visible=%u | drawn(64)=%llu drawn(128)=%llu "
				            "drawn(256)=%llu drawn(1024)=%llu drawn(4096)=%llu drawn(inf)=%u"),
				       *PoolName, bShadowGather ? 1 : 0, R, VisibleQuads,
				       DrawnAt(64), DrawnAt(128), DrawnAt(256), DrawnAt(1024), DrawnAt(4096),
				       VisibleQuads);
			}
		}

		// PASS 1: MERGE ON THE TOLERATED GAP (voxel.Stream.GPUCullMergeGap).
		//
		// This is what that cvar always claimed to do and never did. Its own
		// declaration described the trade exactly -- an off-screen quad costs six
		// vertex invocations and no pixels, a draw costs a state change and a
		// command -- and then nothing read it, because the range-cap merge below
		// was the only merge that existed. A knob that silently ignores you is
		// worse than no knob, so it is wired here rather than deleted: it is the
		// only control over redrawn quads at range counts that are already under
		// the cap, which is the common case.
		//
		// Runs into the cap merge below, which is what still GUARANTEES the
		// element limit; this pass only ever reduces the count it starts from.
		const uint32 MergeGap = uint32(FMath::Max(0, VoxelGpuPoolCull::GMergeGapQuads));
		if (MergeGap > 0 && CulledRanges.Num() > 1)
		{
			TArray<FQuadRange> Merged;
			Merged.Reserve(CulledRanges.Num());
			Merged.Add(CulledRanges[0]);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				FQuadRange& Last = Merged.Last();
				const uint32 LastEnd = Last.First + Last.Count;
				const uint32 Gap = CulledRanges[I].First >= LastEnd ? CulledRanges[I].First - LastEnd : 0u;
				if (Gap <= MergeGap)
				{
					// Max, not +=: runs are sorted by pool offset but a merged
					// range can already extend past the next one's end.
					const uint32 NewEnd = FMath::Max(LastEnd, CulledRanges[I].First + CulledRanges[I].Count);
					Last.Count = NewEnd - Last.First;
				}
				else
				{
					Merged.Add(CulledRanges[I]);
				}
			}
			CulledRanges = MoveTemp(Merged);
		}

		// PASS 2: MERGE TO A DRAW-CALL BUDGET, CHEAPEST GAPS FIRST.
		//
		// The first version merged on a fixed gap threshold and then discarded
		// the whole result if it still exceeded the range cap. That is the wrong
		// shape twice over: the threshold is a magic constant with no relation to
		// what a draw costs, and the discard meant a scene the cull had correctly
		// reduced to 25% visible was drawn in full anyway -- measured
		// visibleQuads=2205034/8808161 with ranges=0, i.e. all the work and none
		// of the benefit.
		//
		// The real objective is: no more than N draws, with as few redrawn
		// off-screen quads as possible. So sort the GAPS between adjacent visible
		// runs, and merge the smallest ones until the range count fits. Merging
		// the smallest gap first is optimal for this objective -- each merge
		// removes exactly one draw and costs exactly that gap in redrawn quads.
		const int32 MaxRanges = VoxelGpuPoolCull::GetMaxRanges();
		if (CulledRanges.Num() > MaxRanges)
		{
			TArray<uint32> Sorted;
			Sorted.Reserve(CulledRanges.Num() - 1);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				const uint32 PrevEnd = CulledRanges[I - 1].First + CulledRanges[I - 1].Count;
				Sorted.Add(CulledRanges[I].First >= PrevEnd ? CulledRanges[I].First - PrevEnd : 0u);
			}
			// The (n - MaxRanges)th smallest gap is the threshold that leaves
			// exactly MaxRanges ranges. Sorting is O(n log n) on a few thousand
			// entries, once per view per frame.
			Sorted.Sort();
			const int32 MergesNeeded = CulledRanges.Num() - MaxRanges;
			const uint32 GapThreshold = Sorted[FMath::Clamp(MergesNeeded - 1, 0, Sorted.Num() - 1)];

			TArray<FQuadRange> Merged;
			Merged.Reserve(MaxRanges);
			Merged.Add(CulledRanges[0]);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				FQuadRange& Last = Merged.Last();
				const uint32 LastEnd = Last.First + Last.Count;
				const uint32 Gap = CulledRanges[I].First >= LastEnd ? CulledRanges[I].First - LastEnd : 0u;
				if (Gap <= GapThreshold)
				{
					const uint32 NewEnd = FMath::Max(LastEnd, CulledRanges[I].First + CulledRanges[I].Count);
					Last.Count = NewEnd - Last.First;
				}
				else
				{
					Merged.Add(CulledRanges[I]);
				}
			}
			CulledRanges = MoveTemp(Merged);
		}

		// Too fragmented to be worth it, or nothing visible at all -- either way
		// the caller falls back to the single full draw. Nothing visible is
		// deliberately NOT treated as "draw nothing": a wrong cull that hides the
		// world is the failure mode this pool is worst at diagnosing, so the
		// conservative branch is the one that draws.
		// Nothing visible -> fall back to the full draw rather than drawing
		// nothing. A wrong cull that hides the world is the failure this pool is
		// worst at diagnosing, so the conservative branch is the one that draws.
		// The range cap no longer needs a bail-out: the merge above satisfies it
		// by construction.
		// THE THREE-STATE EMPTY CASE. See ECullOutcome for why this is not one
		// situation, and why getting it wrong is the largest regression available
		// rather than a missing optimisation.
		if (VisibleQuads == 0)
		{
			CulledRanges.Reset();

			// Did the cap remove geometry this gather would otherwise have drawn?
			// SkippedShadowLevel counts only runs that PASSED the frustum test, so
			// a non-zero value means exactly that -- this cascade has real
			// geometry in it and every last quad of it is above the cap. Drawing
			// nothing is then the correct answer and drawing everything is the
			// catastrophic one.
			OutOutcome = SkippedShadowLevel > 0 ? ECullOutcome::AllCapped
			                                    : ECullOutcome::NothingVisible;

			// Logged unconditionally, not on the periodic sample. Both states are
			// invisible in a picture and one of them is a 13M-quad mistake, so
			// neither may depend on winning a 1-in-601 lottery to be seen. They
			// are also rare by construction, so this cannot spam.
			UE_LOG(LogTemp, Log,
			       TEXT("%s cull: EMPTY (%s) shadowGather=%d cappedRuns=%d cappedQuads=%u frustumSkipped=%d "
			            "-- %s"),
			       *PoolName,
			       OutOutcome == ECullOutcome::AllCapped ? TEXT("all capped") : TEXT("nothing visible"),
			       bShadowGather ? 1 : 0, SkippedShadowLevel, CappedQuads, SkippedFrustum,
			       OutOutcome == ECullOutcome::AllCapped
			           ? TEXT("drawing NOTHING, which is correct: every visible chunk is above GPUShadowMaxLevel")
			           : TEXT("falling back to the FULL POOL draw, which is the conservative answer"));
		}

		// What the merged ranges actually submit, as against VisibleQuads which is
		// the pre-merge total. The gap between the two is the cost of merging --
		// quads redrawn because they sit inside a tolerated gap. Both numbers are
		// needed to tune GPUCullMergeGap; one alone cannot say whether merging is
		// paying for itself.
		uint32 DrawnQuads = 0;
		for (const FQuadRange& R : CulledRanges)
		{
			DrawnQuads += R.Count;
		}

		// Logged PERIODICALLY, not once. The first version logged on the first
		// frame only, which is useless for this diagnostic: at t=0 the pool holds
		// a handful of chunks and the camera may legitimately see none of them,
		// so "visibleQuads=0" reads identically whether the cull is working or
		// rejecting everything. That ambiguity hid a broken cull for a whole
		// verification cycle -- and because the empty case falls back to the full
		// draw, the picture was correct either way and proved nothing.
		//
		// THE PERIOD IS PRIME, AND THAT IS THE ENTIRE POINT OF IT.
		//
		// It was 600, and 600 = 2^3 * 3 * 5^2. Increment() returns the
		// POST-increment value, so the samples fell on gather ordinals
		// 0, 600, 1200, ...; this method runs once per view AND once per shadow
		// cascade, so with G gathers per frame the sampled gather's index within
		// its frame was (600k) mod G -- identically 0 for every plausible G,
		// because every plausible G divides 600.
		//
		// So this line reported gather index 0, and only gather index 0, in
		// every leg ever run: 112 samples across five recorded sessions, all
		// `shadowGather=0`, which was read as "the pool sees no shadow gathers".
		// The tell was on the record and looked like a virtue -- the same
		// `visibleQuads=164534` to the digit across many samples and both
		// straight-down legs, which a rotating sample cannot produce and an
		// aliased one must.
		//
		// 601 is prime, so it shares no factor with any gathers-per-frame count
		// and the sample walks every gather instead of pinning one. EXPECT THIS
		// LINE TO LOOK NOISIER THAN IT USED TO. That is the fix working: the
		// stability it had was the aliasing. `gather=` is printed so the rotation
		// is visible rather than inferred, and the totals -- which is what any
		// quantitative claim should now be read from -- come from the census
		// (voxel.Stream.GPUCullStatsPeriod), which accumulates instead of
		// sampling.
		//
		// DO NOT "FIX" THE NOISE BY ROUNDING THIS BACK TO 600, OR TO ANY OTHER
		// NUMBER WITH SMALL FACTORS. A round period is exactly the bug. If a
		// steadier line is wanted, read the census, which averages over every
		// gather in a window instead of showing you one.
		const int32 GatherOrdinal = CullLogCounter.Increment();
		if (GatherOrdinal % 601 == 1)
		{
			UE_LOG(LogTemp, Log,
			       TEXT("%s cull: gather=%d runs=%d runQuads=%u/%d visibleQuads=%u (%.1f%%) ranges=%d drawnQuads=%u (%.1f%%) "
			            "| mergeGap=%u maxRanges=%d | skipped empty=%d badId=%d aboveHWM=%d hidden=%d frustum=%d "
			            "| shadowGather=%d"),
			       *PoolName, GatherOrdinal - 1, Runs.Num(), RunQuads, NumQuads, VisibleQuads,
			       NumQuads > 0 ? 100.0 * double(VisibleQuads) / double(NumQuads) : 0.0,
			       CulledRanges.Num(), DrawnQuads,
			       NumQuads > 0 ? 100.0 * double(DrawnQuads) / double(NumQuads) : 0.0,
			       MergeGap, MaxRanges,
			       SkippedEmpty, SkippedBadId, SkippedAboveWatermark, SkippedHidden, SkippedFrustum,
			       bShadowGather ? 1 : 0);

			// PER-LEVEL, on its own line so it stays readable at six levels.
			//
			// `pool` is the pool's composition and is view-independent, so it is
			// the same on every gather and is what settles the quads-per-chunk
			// uniformity question. `visible` is pre-cap and per gather, so summing
			// it above a candidate cap gives exactly what that cap would remove
			// from THIS cascade -- no estimate, no uniformity assumption.
			FString PoolByLevel, VisByLevel;
			for (int32 L = 0; L < kMaxLevels; ++L)
			{
				if (PoolQuadsByLevel[L] == 0 && VisibleQuadsByLevel[L] == 0)
				{
					continue;
				}
				PoolByLevel += FString::Printf(TEXT(" L%d=%u"), L, PoolQuadsByLevel[L]);
				VisByLevel += FString::Printf(TEXT(" L%d=%u"), L, VisibleQuadsByLevel[L]);
			}
			UE_LOG(LogTemp, Log,
			       TEXT("%s levels: shadowGather=%d shadowMaxLevel=%d cappedRuns=%d cappedQuads=%u "
			            "| pool:%s | visiblePreCap:%s"),
			       *PoolName, bShadowGather ? 1 : 0, VoxelGpuPoolCull::GShadowMaxLevel,
			       SkippedShadowLevel, CappedQuads, *PoolByLevel, *VisByLevel);
		}
	}

	// Concurrent, for the same reason the cull's working set had to stop being a
	// member: several views run this method at once. A racing ++ on a plain int
	// is undefined behaviour and, more practically, would let the periodic log
	// miss or duplicate its slot.
	mutable FThreadSafeCounter CullLogCounter;
	mutable FThreadSafeCounter CullSpaceLogged;

	// --- the gather census (Wave G / G0) ---------------------------------
	//
	// See VoxelGpuPoolCull::GStatsPeriod for what this is for and why the
	// existing cull log could not answer it.
	//
	// CUMULATIVE since this proxy was created, deliberately. The question is a
	// RATIO -- shadow quads against camera quads -- and a ratio taken over
	// cumulative totals does not care where a reporting window happens to fall,
	// which a per-window average does. A render-state rebuild constructs a new
	// proxy and so resets these; the report says which proxy it came from and
	// how many gathers are behind it, so a reader can see a reset rather than
	// silently averaging across one.
	struct FGatherCensus
	{
		std::atomic<uint64> Gathers{ 0 };
		std::atomic<uint64> SubmittedQuads{ 0 };
		std::atomic<uint64> VisibleQuads{ 0 };

		// Relaxed is correct here and not a shortcut: these counters order
		// nothing and guard nothing, they are only ever read for their own
		// value, and the report is explicitly a snapshot of a live count.
		void Accumulate(uint64 InSubmitted, uint64 InVisible) const
		{
			const_cast<FGatherCensus*>(this)->Gathers.fetch_add(1, std::memory_order_relaxed);
			const_cast<FGatherCensus*>(this)->SubmittedQuads.fetch_add(InSubmitted, std::memory_order_relaxed);
			const_cast<FGatherCensus*>(this)->VisibleQuads.fetch_add(InVisible, std::memory_order_relaxed);
		}
	};
	mutable FGatherCensus CameraCensus;
	mutable FGatherCensus ShadowCensus;

	// Counts what the batch actually asks for, on BOTH branches, rather than
	// re-deriving it from the cull -- the uncull control never runs the cull at
	// all, and it is precisely the config the cost model's other endpoint came
	// from. Reading the submitted elements is the one expression that is true of
	// every path through GetDynamicMeshElements, including the fallbacks.
	static uint64 CountSubmittedQuads(const FMeshBatch& Mesh)
	{
		uint64 Total = 0;
		for (const FMeshBatchElement& Element : Mesh.Elements)
		{
			Total += uint64(Element.NumPrimitives) / 2u;
		}
		return Total;
	}

	// Takes the submitted count EXPLICITLY rather than reading it off an FMeshBatch.
	//
	// It used to take the batch and count its elements, which is correct only
	// AFTER the batch has been populated -- and the all-capped path records a
	// gather that deliberately populates nothing. Collector.AllocateMesh() hands
	// back a RECYCLED FMeshBatch, so Elements[0].NumPrimitives still held a
	// previous gather's value and the census read it as real work.
	//
	// It reported 185,612,113 quads per camera gather against a 13,088,897-quad
	// pool -- impossible on its face, but only because the pool size happened to
	// be in front of me. What killed it was the frame time: the same run measured
	// p50 5.85 ms against 20.73 ms for the un-capped config, i.e. three and a half
	// times FASTER while apparently drawing fourteen times more. The draw was
	// right the whole time; the instrument was reading a stale field.
	//
	// Found only because the all-capped path was deliberately forced with
	// GPUShadowMaxLevel -1, having never once executed in a normal run. Passing
	// the number in removes the ordering requirement rather than documenting it.
	void RecordGather(const FSceneView& View, uint64 InSubmittedQuads, uint32 InVisibleQuads) const
	{
		const int32 Period = VoxelGpuPoolCull::GStatsPeriod;
		if (Period <= 0)
		{
			return;
		}

		const bool bShadowGather = View.GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		(bShadowGather ? ShadowCensus : CameraCensus).Accumulate(InSubmittedQuads, InVisibleQuads);

		if (bShadowGather)
		{
			return;   // report on a camera gather, so the denominator is one frame
		}

		// Reported every Period CAMERA gathers rather than every Period frames.
		// There is no frame counter on this path, and a camera gather is the
		// thing a per-frame figure should be divided by anyway -- so the
		// denominator is stated outright instead of inferred from a frame number
		// this method never sees.
		const uint64 CameraGathers = CameraCensus.Gathers.load(std::memory_order_relaxed);
		if (CameraGathers == 0 || (CameraGathers % uint64(Period)) != 0)
		{
			return;
		}

		const uint64 CamQuads = CameraCensus.SubmittedQuads.load(std::memory_order_relaxed);
		const uint64 ShadowGathers = ShadowCensus.Gathers.load(std::memory_order_relaxed);
		const uint64 ShadowQuads = ShadowCensus.SubmittedQuads.load(std::memory_order_relaxed);
		const uint64 CamVisible = CameraCensus.VisibleQuads.load(std::memory_order_relaxed);

		// WINDOWED AS WELL AS CUMULATIVE, and the window is the number to read.
		//
		// The cumulative figures start at proxy creation, which is BEFORE the
		// cascade has streamed in -- so they average a nearly-empty pool together
		// with a settled one and understate both quad counts. That dilution is
		// not identical between two configs, because their streaming trajectories
		// are not identical, so it does not cancel in the delta the experiment is
		// built on. The last window of a settled leg contains no fill-in frames at
		// all, which is what the pre-registered rule is written against.
		const uint64 WinCamGathers = CameraGathers - LastCamGathers.exchange(CameraGathers, std::memory_order_relaxed);
		const uint64 WinCamQuads = CamQuads - LastCamQuads.exchange(CamQuads, std::memory_order_relaxed);
		const uint64 WinShadowGathers = ShadowGathers - LastShadowGathers.exchange(ShadowGathers, std::memory_order_relaxed);
		const uint64 WinShadowQuads = ShadowQuads - LastShadowQuads.exchange(ShadowQuads, std::memory_order_relaxed);
		const uint64 WinCamVisible = CamVisible - LastCamVisible.exchange(CamVisible, std::memory_order_relaxed);

		if (WinCamGathers == 0)
		{
			return;
		}

		const uint64 WinTotalQuads = WinCamQuads + WinShadowQuads;

		// A MEASURED ZERO, SAID OUT LOUD.
		//
		// shadowGathers == 0 is this experiment's best case -- it makes S_delta
		// exactly 1, which leaves Wave A's cost model standing unmodified. But an
		// absent count and a zero count look identical in a log, and that is
		// precisely how the original aliasing hid: 112 samples all reading
		// shadowGather=0 were taken as "no shadow gathers happen" when they were
		// really "this instrument cannot see them". So the zero is asserted on its
		// own line, by a counter that increments on every gather of either kind,
		// rather than being inferred from lines that are not there.
		if (WinShadowGathers == 0)
		{
			UE_LOG(LogTemp, Log,
			       TEXT("%s census[window]: shadowGathers=0 MEASURED (not absent) over %llu camera gathers "
			            "-- this proxy submitted no shadow-cascade draws in this window, so S_delta = 1 "
			            "and a camera-only quad count is the whole frame's quad count"),
			       *PoolName, WinCamGathers);
		}

		// S is the whole point of the experiment: the factor by which a
		// main-view-only quad count understates what the frame actually drew.
		// Wave A's ~1.19 us/1000 quads was a whole-frame delta-p50 over a
		// main-view-only delta-quads, so the true slope is 1.19 / S and the
		// saving compaction can recover on the camera view scales the same way.
		//
		// NOTE this is S at ONE config. The quantity the rule is written against
		// is S_delta, formed from the DIFFERENCE between uncull and cull at a
		// pose -- computed off two legs, not read off one line. S here is the
		// per-config ingredient and a sanity check, not the verdict.
		const double S = WinCamQuads > 0 ? double(WinTotalQuads) / double(WinCamQuads) : 0.0;

		UE_LOG(LogTemp, Log,
		       TEXT("%s census[window]: cameraGathers=%llu shadowGathers=%llu (%.2f per camera gather) | "
		            "quads/cameraGather: camera=%.0f shadow=%.0f total=%.0f | S=%.3f | "
		            "visible/cameraGather camera=%.0f | poolQuads=%d cull=%d "
		            "| cumulative: camGathers=%llu shadowGathers=%llu camQuads=%llu shadowQuads=%llu"),
		       *PoolName, WinCamGathers, WinShadowGathers,
		       double(WinShadowGathers) / double(WinCamGathers),
		       double(WinCamQuads) / double(WinCamGathers),
		       double(WinShadowQuads) / double(WinCamGathers),
		       double(WinTotalQuads) / double(WinCamGathers),
		       S,
		       double(WinCamVisible) / double(WinCamGathers),
		       NumQuads, VoxelGpuPoolCull::IsEnabled() ? 1 : 0,
		       CameraGathers, ShadowGathers, CamQuads, ShadowQuads);
	}

	// Previous report's cumulative values, so each report can state its own
	// window. Only ever touched on a reporting camera gather.
	mutable std::atomic<uint64> LastCamGathers{ 0 };
	mutable std::atomic<uint64> LastCamQuads{ 0 };
	mutable std::atomic<uint64> LastShadowGathers{ 0 };
	mutable std::atomic<uint64> LastShadowQuads{ 0 };
	mutable std::atomic<uint64> LastCamVisible{ 0 };



	FBufferRHIRef QuadBuffer, ChunkIdBuffer, OriginBuffer, ParamsBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV, ChunkIdSRV, OriginSRV, ParamsSRV;

	mutable FThreadSafeCounter ElementsLogged;
	// Latches the one MULTI-BATCH line. Separate from ElementsLogged because that
	// one fires on the FIRST gather, which may well be an uncull or an empty one
	// -- the batch split is only observable on a gather that actually produced
	// ranges, so it needs its own latch or the line never prints.
	mutable FThreadSafeCounter BatchesLogged;
	int32 NumQuads = 0;      // drawn
	int32 BufferQuads = 0;   // allocated
	int32 NumChunks = 0;
	// Headroom in the chunk table so ordinary streaming churn never has to
	// rebuild the render state just to add one more chunk.
	int32 MaxChunks = 0;

	FMaterialRenderProxy* MaterialProxy = nullptr;
	FMaterialRelevance MaterialRelevance;
};

UVoxelGpuPoolComponent::UVoxelGpuPoolComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CastShadow = true;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UVoxelGpuPoolComponent::InitPool(uint32 CapacityQuads)
{
	Pool.Init(CapacityQuads);
	PooledQuads.SetNumZeroed(int32(CapacityQuads));
	QuadChunkIds.SetNumZeroed(int32(CapacityQuads));

	// Reserve the hidden chunk at index 0. Everything freed points here.
	ChunkOrigins.Reset();
	ChunkOrigins.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
	ChunkParams.Reset();
	// Hidden chunk: neutral climate, and a surface height so far below the chunk
	// that the shader's surface-proximity gate reads "at the surface" -- matching
	// BuildChunkVertexData's own fallback when no world subsystem is available.
	// Nothing is drawn from this entry (scale 0 collapses it), so the values only
	// have to be harmless.
	ChunkParams.Add(FVector4f(0.5f, 0.5f, kNoSurfaceGate, 0.0f));
	FreeChunkIds.Reset();

	Allocations.Reset();
	AllocationChunkIds.Reset();
	FreeHandles.Reset();
	NumLiveChunks = 0;
	LocalBounds = FBox(ForceInit);
	DirtyQuadRanges.Reset();
	// Every allocation this pool knew about is gone, so a pending write would
	// target an offset that now belongs to nobody. Dropped with the rest of the
	// state, which also releases the payloads' GPU memory. GpuDirectWrites is
	// NOT reset, for the same reason AllocFailureCount is not: it answers "what
	// did this RUN do", not "what is happening right now".
	PendingGpuWrites.Reset();
	bChunkTableDirty = false;
	bRunsDirty = false;
}

// See the declaration for why this is safe to call unconditionally: it
// costs one bool check once latched, and it is latched the first time either
// trigger fires so this is the only place a saturated pool ever logs loud.
//
// 95% RATHER THAN 100%, because 100% is too late to be a warning -- by the
// time capacity is exactly exhausted, chunks have already started silently
// failing to allocate (that is the OTHER trigger, below), and the whole
// point of a percentage trip is to fire while there is still time to resize
// before the run reaches it. Resident is Capacity - GetFreeQuads() rather
// than a running total kept alongside NumLiveChunks, because the allocator
// is the one source of truth for it and a second counter can only drift
// from that one.
void UVoxelGpuPoolComponent::MaybeLogSaturation(bool bAllocFailed)
{
	if (bSaturationErrorLogged)
	{
		return;
	}

	const uint32 Capacity = Pool.GetCapacityQuads();
	const uint32 Resident = Capacity - Pool.GetFreeQuads();
	if (!bAllocFailed && (Capacity == 0 || uint64(Resident) * 100 < uint64(Capacity) * 95))
	{
		return;   // neither trigger has fired yet
	}

	bSaturationErrorLogged = true;
	const double Pct = Capacity > 0 ? 100.0 * double(Resident) / double(Capacity) : 0.0;
	UE_LOG(LogTemp, Error,
	       TEXT("%s: POOL SATURATION -- capacity %u quads, %u resident (%.1f%%)%s. Chunks will silently ")
	       TEXT("fail to appear; resize with -VoxelPoolCapacityQuads or shrink the cascade."),
	       *PoolName, Capacity, Resident, Pct,
	       bAllocFailed ? TEXT(" -- an allocation just failed") : TEXT(" -- crossed the 95% watch line"));
}

// S1-2. The single acquire path AddChunk and AddChunkFromGpu both call --
// see FreeHandles for why there must be only one of these rather than each
// caller re-implementing "pop if available, else append".
int32 UVoxelGpuPoolComponent::AcquireAllocationHandle(const FVoxelGpuPoolAllocation& Alloc, uint32 ChunkId)
{
	if (FreeHandles.Num() > 0)
	{
		const int32 Handle = FreeHandles.Pop(EAllowShrinking::No);
		Allocations[Handle] = Alloc;
		AllocationChunkIds[Handle] = ChunkId;
		check(AllocationChunkIds.Num() == Allocations.Num());
		return Handle;
	}
	const int32 Handle = Allocations.Add(Alloc);
	AllocationChunkIds.Add(ChunkId);
	check(AllocationChunkIds.Num() == Allocations.Num());
	return Handle;
}

int32 UVoxelGpuPoolComponent::AddChunk(const TArray<uint64>& InQuads,
                                       const FVector3f& OriginUU, int32 Level,
                                       const FVector4f& Params)
{
	check(Pool.GetCapacityQuads() > 0);   // InitPool first

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		// Out of CONTIGUOUS room, which is not the same as out of space --
		// see FVoxelGpuGeometryPool. The caller decides whether to compact.
		//
		// Counted as well as logged: the caller's only signal is INDEX_NONE, and
		// every caller in the tree treats that as "no geometry for this chunk"
		// and moves on. See GetAllocFailureCount for why a per-occurrence warning
		// is not enough on its own.
		++AllocFailureCount;
		AllocFailureQuads += InQuads.Num();
		// The one loud signal -- see MaybeLogSaturation. Fires once, ever, for
		// this pool; every call after the first is a no-op.
		MaybeLogSaturation(/*bAllocFailed=*/true);
		// Warn on the first, then at powers of ten: a pool that has genuinely run
		// out fails on nearly every subsequent chunk, and a per-chunk warning at
		// that rate buries the streaming log it would be diagnosed from.
		if (FMath::IsPowerOfTwo(AllocFailureCount) || AllocFailureCount == 1)
		{
			UE_LOG(LogTemp, Warning,
			       TEXT("%s: no room for %d quads (%u free, largest run %u) -- "
			            "GEOMETRY DROPPED, failure %lld of this run (%lld quads total)"),
			       *PoolName, InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun(),
			       (long long)AllocFailureCount, (long long)AllocFailureQuads);
		}
		return INDEX_NONE;
	}

	// Resident quads just grew; check the OTHER trigger for the loud signal --
	// a pool can coast up to and across the 95% line without a single
	// allocation ever failing, if nothing has fragmented it. No-op once
	// latched, same as the call in the failure branch above.
	MaybeLogSaturation(/*bAllocFailed=*/false);

	const float Scale = float(1 << Level);
	// Re-use a table entry a removed chunk gave back before appending a new one.
	// See FreeChunkIds for why the append-only version was a cliff rather than a
	// slow leak on any pool with real churn.
	uint32 ChunkId;
	if (FreeChunkIds.Num() > 0)
	{
		ChunkId = FreeChunkIds.Pop(EAllowShrinking::No);
		ChunkOrigins[int32(ChunkId)] = FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale);
		ChunkParams[int32(ChunkId)] = Params;
	}
	else
	{
		ChunkId = uint32(ChunkOrigins.Num());
		ChunkOrigins.Add(FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale));
		ChunkParams.Add(Params);
	}

	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}

	const int32 Handle = AcquireAllocationHandle(Alloc, ChunkId);
	++NumLiveChunks;

	// Grow the bounds by this chunk's extent. A chunk's quads never leave its
	// own ChunkEdgeVoxels cube, scaled by the mip level (32 voxels for a terrain
	// render chunk, 8 for a vxc::WaterBrick8).
	const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Scale;
	LocalBounds += FBox(
		FVector(OriginUU.X, OriginUU.Y, OriginUU.Z),
		FVector(OriginUU.X + EdgeUU, OriginUU.Y + EdgeUU, OriginUU.Z + EdgeUU));

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	bChunkTableDirty = true;
	bRunsDirty = true;
	PushUpdatesToProxy();
	UpdateBounds();
	return Handle;
}

bool UVoxelGpuPoolComponent::IsGpuWritable() const
{
	return PoolBuffers.IsValid() && PoolBuffers->GpuWritable.GetValue() != 0;
}

int32 UVoxelGpuPoolComponent::AddChunkFromGpu(const FVoxelGpuQuadPayloadRef& Src, uint32 NumQuads,
                                              const FVector3f& OriginUU, int32 Level,
                                              const FVector4f& Params)
{
	check(Pool.GetCapacityQuads() > 0);   // InitPool first

	if (!Src.IsValid() || NumQuads == 0)
	{
		// A zero-quad chunk allocates nothing and has no payload -- the mesh job
		// short-circuits it a phase earlier. Reaching here with one is a caller
		// bug, not a pool state, so it fails visibly rather than silently
		// allocating an empty range.
		return INDEX_NONE;
	}
	if (!IsGpuWritable())
	{
		// The buffers do not exist yet, so a write would have nowhere to land.
		// Refuse BEFORE allocating: an allocation whose write is dropped leaves a
		// live chunk-table entry naming a range full of zeros, which draws as
		// nothing and reports as success.
		return INDEX_NONE;
	}

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(NumQuads);
	if (!Alloc.IsValid())
	{
		// Byte-for-byte the AddChunk failure path, counted in the same counters.
		// A full pool drops geometry identically whichever mesher produced it,
		// and splitting the accounting would make GetAllocFailureCount stop
		// answering "did this run ever drop geometry".
		++AllocFailureCount;
		AllocFailureQuads += int64(NumQuads);
		if (FMath::IsPowerOfTwo(AllocFailureCount) || AllocFailureCount == 1)
		{
			UE_LOG(LogTemp, Warning,
			       TEXT("%s: no room for %u GPU-meshed quads (%u free, largest run %u) -- "
			            "GEOMETRY DROPPED, failure %lld of this run (%lld quads total)"),
			       *PoolName, NumQuads, Pool.GetFreeQuads(), Pool.GetLargestFreeRun(),
			       (long long)AllocFailureCount, (long long)AllocFailureQuads);
		}
		return INDEX_NONE;
	}

	const float Scale = float(1 << Level);
	uint32 ChunkId;
	if (FreeChunkIds.Num() > 0)
	{
		ChunkId = FreeChunkIds.Pop(EAllowShrinking::No);
		ChunkOrigins[int32(ChunkId)] = FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale);
		ChunkParams[int32(ChunkId)] = Params;
	}
	else
	{
		ChunkId = uint32(ChunkOrigins.Num());
		ChunkOrigins.Add(FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale));
		ChunkParams.Add(Params);
	}

	// THE ONE THING THIS DOES NOT DO. AddChunk writes PooledQuads and
	// QuadChunkIds here and then calls MarkQuadsDirty, which is what schedules
	// the upload. Neither happens: the quads are already on the GPU, the ids are
	// written by the same compute pass that copies them, and marking the range
	// dirty would upload the shadow's ZEROS straight over the geometry that pass
	// is about to write. See PooledQuads for why leaving the shadow zeroed is the
	// safe direction rather than merely the cheap one.
	//
	// S1-1: AND IT MUST ACTIVELY UN-MARK THE RANGE, not merely decline to mark it.
	// Not marking is sufficient only while every mutation publishes on its own.
	// Batched, a RemoveChunk earlier in the same tick may already have marked this
	// exact range dirty (it stamps hidden ids over the quads it frees), and that
	// interval would ride the same command as this write -- landing AFTER it,
	// because the copy passes are recorded first. The freed range's stale ids
	// would be written over geometry the compute pass just produced, and the
	// result is terrain that is invisible and reports as loaded.
	//
	// Unconditional, not gated on GVoxelPoolBatchPublish: with batching off the
	// list cannot contain an overlap at this point, so this is a no-op that costs
	// one length check -- and a subtract that only runs when a flag is set is a
	// subtract that is untested every time the flag is off.
	UnmarkQuadsDirty(Alloc.Offset, NumQuads);

	const int32 Handle = AcquireAllocationHandle(Alloc, ChunkId);
	++NumLiveChunks;

	const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Scale;
	LocalBounds += FBox(
		FVector(OriginUU.X, OriginUU.Y, OriginUU.Z),
		FVector(OriginUU.X + EdgeUU, OriginUU.Y + EdgeUU, OriginUU.Z + EdgeUU));

	PendingGpuWrites.Add(FPendingGpuWrite{ Src, Alloc.Offset, NumQuads, ChunkId });
	++GpuDirectWrites;

	bChunkTableDirty = true;
	bRunsDirty = true;
	// Drains PendingGpuWrites into the SAME render command as the table update,
	// with the copy passes recorded first -- see FPendingGpuWrite.
	PushUpdatesToProxy();
	UpdateBounds();
	return Handle;
}

void UVoxelGpuPoolComponent::RemoveChunk(int32 Handle)
{
	RemoveChunkInternal(Handle, /*bRecycleChunkId=*/true);
}

void UVoxelGpuPoolComponent::RemoveChunkInternal(int32 Handle, bool bRecycleChunkId)
{
	if (!Allocations.IsValidIndex(Handle) || !Allocations[Handle].IsValid())
	{
		return;
	}

	const FVoxelGpuPoolAllocation Alloc = Allocations[Handle];

	// Point the freed quads at the hidden chunk (scale 0) so they collapse to
	// a degenerate point rather than continuing to draw stale geometry.
	for (uint32 I = 0; I < Alloc.NumQuads; ++I)
	{
		QuadChunkIds[int32(Alloc.Offset + I)] = kHiddenChunkId;
	}

	// Only now is the table entry unreferenced by any quad, which is what makes
	// handing it back safe.
	if (bRecycleChunkId && AllocationChunkIds.IsValidIndex(Handle))
	{
		const uint32 ChunkId = AllocationChunkIds[Handle];
		if (ChunkId != kHiddenChunkId)
		{
			// Neutralise the entry as well as freeing it: if anything ever does
			// reach it before it is re-issued, scale 0 collapses it rather than
			// drawing a stale origin.
			ChunkOrigins[int32(ChunkId)] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			FreeChunkIds.Add(ChunkId);
			bChunkTableDirty = true;
		}
	}

	Pool.Free(Alloc);
	Allocations[Handle] = FVoxelGpuPoolAllocation{};
	--NumLiveChunks;

	// S1-2: hand the slot back for AcquireAllocationHandle to reuse -- see
	// FreeHandles. Gated on bRecycleChunkId for the SAME reason the ChunkId
	// recycling just above is: UpdateChunk's realloc branch calls this with
	// bRecycleChunkId=false because it is not really removing this chunk, it is
	// about to write Allocations[Handle] itself a few lines later, reusing this
	// EXACT handle. Pushing it here would let AcquireAllocationHandle hand the
	// same index to an unrelated chunk before that write lands -- nothing
	// currently runs between RemoveChunkInternal returning and that write inside
	// UpdateChunk's single synchronous call, but recycling the handle anyway
	// would be a landmine for the next person who inserts something in between,
	// and the failure is two live chunks silently aliasing one pool slot.
	if (bRecycleChunkId && GVoxelPoolRecycleHandles != 0)
	{
		FreeHandles.Add(Handle);
	}

	// The allocation set changed even when the table entry did not (the realloc
	// caller keeps its entry), so the runs are stale either way.
	bRunsDirty = true;

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	PushUpdatesToProxy();
}

void UVoxelGpuPoolComponent::ClearChunks()
{
	if (Pool.GetCapacityQuads() > 0)
	{
		InitPool(Pool.GetCapacityQuads());
	}
	MarkRenderStateDirty();
	UpdateBounds();
}


// How close two dirty runs must be before they are merged into one.
//
// The trade is real in both directions: merging uploads quads nobody wrote,
// while not merging costs an extra LockBuffer/UnlockBuffer per run. The old
// code sat at one extreme (always merge, one run, unbounded waste).
//
// DEFAULT 0 = MERGE ONLY WHAT ACTUALLY TOUCHES. That is the setting that makes
// the path-2 correctness property hold exactly: a merged run can span quads the
// CPU did not write, which for a non-zero gap could include a GPU-written
// range. Raise it only for measured upload-call savings on a CPU-only pool, and
// never above the smallest GPU-written allocation.
//
// This knob is PROVEN LIVE, not assumed: voxel.Stream.PoolUploadStats prints
// the run count and bytes per update, so changing the gap visibly changes both.
// That check exists because this project already shipped
// voxel.Stream.GPUCullMergeGap, which was declared, documented, and never read
// by anything -- a knob that silently ignores you is worse than no knob.
static int32 GVoxelPoolDirtyMergeGap = 0;
static FAutoConsoleVariableRef CVarVoxelPoolDirtyMergeGap(
	TEXT("voxel.Stream.PoolDirtyMergeGap"),
	GVoxelPoolDirtyMergeGap,
	TEXT("Quads of gap tolerated when merging two dirty pool runs into one upload. ")
	TEXT("0 (default) merges only touching/overlapping runs. Higher trades wasted upload ")
	TEXT("bytes for fewer buffer locks. Verify with voxel.Stream.PoolUploadStats 1."),
	ECVF_Default);

// Prints what each incremental upload actually cost. A mechanism count, not a
// frame time -- it is not vulnerable to contention or to the clamp, so it is
// safe to quote from a shared box.
static int32 GVoxelPoolUploadStats = 0;
static FAutoConsoleVariableRef CVarVoxelPoolUploadStats(
	TEXT("voxel.Stream.PoolUploadStats"),
	GVoxelPoolUploadStats,
	TEXT("1 = log runs and bytes uploaded per incremental pool update. This is how the ")
	TEXT("dirty-run merge gap is shown to be live rather than decorative."),
	ECVF_Default);

void UVoxelGpuPoolComponent::MarkQuadsDirty(uint32 First, uint32 Count)
{
	if (Count == 0)
	{
		return;
	}
	const uint32 Last = First + Count - 1;

	// Insert in ascending First order, then sweep once and coalesce. The list
	// holds one entry per chunk touched since the last upload, which the apply
	// budget already bounds to a handful per frame, so linear is right here and
	// an interval tree would be strictly more code for no measurable gain.
	int32 Index = 0;
	while (Index < DirtyQuadRanges.Num() && DirtyQuadRanges[Index].First < First)
	{
		++Index;
	}
	DirtyQuadRanges.Insert(FDirtyRange{ First, Last, true }, Index);

	const uint64 Gap = uint64(FMath::Max(0, GVoxelPoolDirtyMergeGap));
	for (int32 I = 0; I + 1 < DirtyQuadRanges.Num(); )
	{
		FDirtyRange& A = DirtyQuadRanges[I];
		const FDirtyRange& B = DirtyQuadRanges[I + 1];
		// Touching, overlapping, or within the gap. +1 makes [0,3] and [4,7]
		// adjacent rather than separate at Gap 0.
		if (uint64(B.First) <= uint64(A.Last) + 1ull + Gap)
		{
			A.Last = FMath::Max(A.Last, B.Last);
			DirtyQuadRanges.RemoveAt(I + 1, EAllowShrinking::No);
		}
		else
		{
			++I;
		}
	}
}

// The subtract. See the declaration for WHY this exists -- it is what makes
// S1-1's batching safe, and without it batching silently corrupts geometry.
void UVoxelGpuPoolComponent::UnmarkQuadsDirty(uint32 First, uint32 Count)
{
	if (Count == 0 || DirtyQuadRanges.Num() == 0)
	{
		return;
	}
	const uint32 Last = First + Count - 1;

	bool bTouchedAnything = false;
	// Walk backwards: the two-interval split case inserts, and inserting behind
	// the cursor is what keeps this a single pass. Same linear-is-right argument
	// as MarkQuadsDirty -- the list holds one entry per CPU-written chunk since
	// the last flush.
	for (int32 I = DirtyQuadRanges.Num() - 1; I >= 0; --I)
	{
		FDirtyRange& R = DirtyQuadRanges[I];
		if (R.Last < First || R.First > Last)
		{
			continue; // disjoint
		}

		// Quads actually reclaimed from the upload, for the counter. Computed
		// before the interval is mutated.
		const uint32 OverlapFirst = FMath::Max(R.First, First);
		const uint32 OverlapLast = FMath::Min(R.Last, Last);
		DirtyOverlapQuadsResolved += int64(OverlapLast - OverlapFirst + 1);
		bTouchedAnything = true;

		const bool bKeepHead = R.First < First;   // [R.First, First-1] survives
		const bool bKeepTail = R.Last > Last;     // [Last+1, R.Last]   survives

		if (bKeepHead && bKeepTail)
		{
			// The removed range is strictly inside this one: split in two.
			const FDirtyRange Tail{ Last + 1, R.Last, true };
			R.Last = First - 1;
			DirtyQuadRanges.Insert(Tail, I + 1);
		}
		else if (bKeepHead)
		{
			R.Last = First - 1;
		}
		else if (bKeepTail)
		{
			R.First = Last + 1;
		}
		else
		{
			// Fully covered.
			DirtyQuadRanges.RemoveAt(I, EAllowShrinking::No);
		}
	}

	if (bTouchedAnything)
	{
		++DirtyOverlapsResolved;
	}
}

// Records every pending write's copy pass into GraphBuilder. RENDER THREAD.
//
// Shared by the two places a flush can happen -- folded into the incremental
// update's command, or standalone when PushUpdatesToProxy returns before
// building one -- so the two cannot drift on the thing that matters: what the
// pass is given and in what order.
//
// The render-thread re-check of the buffers is not belt-and-braces for the
// game-thread IsGpuWritable() gate. It is the only check that CANNOT be stale,
// and it is where a dropped write is detectable at all.
static void VoxelGpuPoolAddWritePasses(FRDGBuilder& GraphBuilder,
                                       const FVoxelGpuPoolBuffersRef& Buffers,
                                       const TArray<UVoxelGpuPoolComponent::FPendingGpuWrite>& Writes,
                                       const TCHAR* PoolName)
{
	if (!Buffers.IsValid())
	{
		// Cannot happen -- GetOrCreatePoolBuffers always returns a holder and the
		// command captured it by shared reference -- but a null here would be a
		// silent total loss of geometry, so it is stated rather than assumed.
		UE_LOG(LogTemp, Error, TEXT("%s: direct GPU writes DROPPED — no buffer holder"), PoolName);
		return;
	}
	if (!Buffers->QuadPooled.IsValid() || !Buffers->ChunkIdPooled.IsValid())
	{
		Buffers->DroppedWrites.Add(Writes.Num());
		UE_LOG(LogTemp, Error,
		       TEXT("%s: %d direct GPU write(s) DROPPED — the pool buffers are not there. Those chunks hold "
		            "an allocated range and a live table entry with no geometry in it, i.e. terrain that is "
		            "missing and reports as loaded."),
		       PoolName, Writes.Num());
		return;
	}

	FRDGBufferRef DstQuads = GraphBuilder.RegisterExternalBuffer(Buffers->QuadPooled, TEXT("VoxelGpuPool.Quads"));
	FRDGBufferRef DstIds = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkIdPooled, TEXT("VoxelGpuPool.ChunkIds"));

	for (const UVoxelGpuPoolComponent::FPendingGpuWrite& Write : Writes)
	{
		if (!Write.Src.IsValid() || !Write.Src->Quads.IsValid() || Write.NumQuads == 0)
		{
			Buffers->DroppedWrites.Increment();
			UE_LOG(LogTemp, Error,
			       TEXT("%s: direct GPU write for chunk %u DROPPED — payload has no buffer. %u quads at pool "
			            "offset %u are allocated and empty."),
			       PoolName, Write.ChunkId, Write.NumQuads, Write.DstFirst);
			continue;
		}

		FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(Write.Src->Quads, TEXT("VoxelGpuPool.SrcQuads"));
		VoxelGpuWorldGen::AddQuadPoolWritePass(GraphBuilder, DstQuads, DstIds, Src,
		                                       Write.Src->SrcFirst, Write.DstFirst,
		                                       Write.NumQuads, Write.ChunkId);
	}
}

TArray<UVoxelGpuPoolComponent::FPendingGpuWrite> UVoxelGpuPoolComponent::TakePendingGpuWrites()
{
	TArray<FPendingGpuWrite> Writes = MoveTemp(PendingGpuWrites);
	PendingGpuWrites.Reset();
	return Writes;
}

UVoxelGpuPoolComponent::FPoolPushStats UVoxelGpuPoolComponent::GetAndResetPushStats()
{
	FPoolPushStats Out = PushStats;
	PushStats = FPoolPushStats{};

	// The render-thread half. Reset() on FThreadSafeCounter64 returns the old
	// value and zeroes atomically, so a RebuildRunBounds landing mid-drain is
	// counted in exactly one window rather than lost or double-counted.
	//
	// These lag the game-thread figures: they report whatever the render thread
	// had finished by the time this ran, which is a frame or two behind the
	// publications that caused them. Fine for a 5 s census, wrong for anything
	// finer -- see FPoolPushStats.
	if (PoolBuffers.IsValid())
	{
		const int64 Cycles = PoolBuffers->RunBoundsCycles.Reset();
		Out.RunBoundsCalls = PoolBuffers->RunBoundsCalls.Reset();
		Out.RunBoundsRunsWalked = PoolBuffers->RunBoundsRunsWalked.Reset();
		// Cycles -> ms once, here, rather than per call on the render thread.
		Out.RunBoundsMs = FPlatformTime::ToMilliseconds64(uint64(Cycles));
	}
	return Out;
}

void UVoxelGpuPoolComponent::FlushGpuWritesStandalone(TArray<FPendingGpuWrite>&& Writes)
{
	if (Writes.Num() == 0)
	{
		return;
	}
	// For the PushUpdatesToProxy branches that return before building their own
	// command. Those branches call MarkRenderStateDirty, whose render-state
	// rebuild is enqueued later (end of frame), so this command still lands
	// first -- which is the order the drawable-after-written rule needs.
	ENQUEUE_RENDER_COMMAND(VoxelGpuPoolDirectWrite)(
		[Buffers = GetOrCreatePoolBuffers(), Writes = MoveTemp(Writes),
		 Name = PoolName](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		VoxelGpuPoolAddWritePasses(GraphBuilder, Buffers, Writes, *Name);
		GraphBuilder.Execute();
	});
}

UVoxelGpuPoolComponent::FScopedBatch::FScopedBatch(UVoxelGpuPoolComponent* InPool)
	: Pool(InPool)
{
	if (UVoxelGpuPoolComponent* P = Pool.Get())
	{
		++P->BatchDepth;
	}
}

UVoxelGpuPoolComponent::FScopedBatch::~FScopedBatch()
{
	UVoxelGpuPoolComponent* P = Pool.Get();
	if (P == nullptr)
	{
		// The component was collected inside the scope. Nothing to flush into,
		// and nothing to leak -- the pending writes died with it.
		return;
	}

	P->BatchDepth = FMath::Max(0, P->BatchDepth - 1);
	if (P->BatchDepth > 0)
	{
		return; // inner scope; the outermost one publishes
	}

	// FLUSH ON THE OUTERMOST CLOSE, AND ONLY IF SOMETHING ASKED FOR ONE.
	// bFlushPending is what makes an idle tick free: a scope opened around a
	// drain that applied nothing costs one increment, one decrement and one bool
	// test, not a publication of an unchanged table.
	if (P->bFlushPending)
	{
		P->bFlushPending = false;
		P->FlushUpdatesToProxy();
	}
}

// THE DEFER POINT. Every mutator still calls this and none of them know about
// batching; whether it publishes now or records that a publication is owed is
// decided here and nowhere else.
void UVoxelGpuPoolComponent::PushUpdatesToProxy()
{
	if (GVoxelPoolBatchPublish != 0 && BatchDepth > 0)
	{
		bFlushPending = true;
		return;
	}
	FlushUpdatesToProxy();
}

void UVoxelGpuPoolComponent::FlushUpdatesToProxy()
{
	// Taken FIRST, on every path out of this function. A pending write that
	// survived a call to PushUpdatesToProxy would be flushed against a later
	// frame's state, after the range it targets had already been published as
	// drawable.
	TArray<FPendingGpuWrite> GpuWrites = TakePendingGpuWrites();

	// No live proxy yet: the CPU arrays are the source of truth and the proxy
	// will pick them up whole when it is created.
	if (LiveProxy == nullptr)
	{
		FlushGpuWritesStandalone(MoveTemp(GpuWrites));
		MarkRenderStateDirty();
		DirtyQuadRanges.Reset();
		bChunkTableDirty = false;
		bRunsDirty = false;
		return;
	}

	// The chunk table outgrew its headroom -- rebuild rather than overrun it.
	if (ChunkOrigins.Num() > LiveProxy->GetMaxChunks())
	{
		FlushGpuWritesStandalone(MoveTemp(GpuWrites));
		MarkRenderStateDirty();
		DirtyQuadRanges.Reset();
		bChunkTableDirty = false;
		bRunsDirty = false;
		return;
	}

	// Flatten the dirty runs into one staging payload. Several small runs beat
	// one span covering all the untouched geometry between them -- see
	// DirtyQuadRanges' comment for why that is both a correctness and a
	// bandwidth argument.
	TArray<FVoxelQuadUploadRun> UploadRuns;
	uint32 TotalQuadsToUpload = 0;
	for (const FDirtyRange& R : DirtyQuadRanges)
	{
		const uint32 RunCount = R.Last - R.First + 1;
		UploadRuns.Add(FVoxelQuadUploadRun{ R.First, RunCount, TotalQuadsToUpload });
		TotalQuadsToUpload += RunCount;
	}
	const int32 NewNumQuads = int32(Pool.GetHighWaterMark());
	// The runs travel on the table update, so a run-only change has to send the
	// table too. It is two small vectors per chunk; the quad buffer, which is the
	// expensive one, is still written by range.
	const bool bTableDirty = bChunkTableDirty || bRunsDirty;

	FVoxelGpuPoolSceneProxy* Proxy = LiveProxy;

	// Copy ONLY the dirty slice, never the whole pool.
	//
	// This used to hand the render command a copy of the entire PooledQuads and
	// QuadChunkIds arrays -- at cascade capacity that is ~170 MB of memcpy per
	// chunk added. It did not corrupt anything, which is why it survived the
	// small-scale tests; it simply ate the streaming apply budget alive. The
	// near ring stalled at 178 of ~1900 chunks while every coarser ring filled
	// normally, and the visible symptom was the coarse rings' exposed interiors
	// where the missing fine terrain should have been.
	//
	// Copying a full buffer per incremental update is precisely the cost
	// ADR-0006 exists to remove, so getting it wrong here forfeits the win the
	// pool was built for.
	TArray<uint64> QuadsSlice;
	TArray<uint32> IdsSlice;
	QuadsSlice.Reserve(int32(TotalQuadsToUpload));
	IdsSlice.Reserve(int32(TotalQuadsToUpload));
	for (const FVoxelQuadUploadRun& Run : UploadRuns)
	{
		QuadsSlice.Append(PooledQuads.GetData() + Run.First, int32(Run.Count));
		IdsSlice.Append(QuadChunkIds.GetData() + Run.First, int32(Run.Count));
	}

	if (GVoxelPoolUploadStats != 0 && UploadRuns.Num() > 0)
	{
		// Span-equivalent cost, for comparison: what the single-span version
		// would have uploaded for exactly this set of dirty chunks.
		const uint32 SpanFirst = UploadRuns[0].First;
		const uint32 SpanLast = UploadRuns.Last().First + UploadRuns.Last().Count - 1;
		const uint32 SpanQuads = SpanLast - SpanFirst + 1;
		UE_LOG(LogTemp, Log,
		       TEXT("%s upload: %d run(s), %u quads (%u KB) — a single span would have been "
		            "%u quads (%u KB), gap=%d"),
		       *PoolName, UploadRuns.Num(), TotalQuadsToUpload,
		       (TotalQuadsToUpload * uint32(sizeof(uint64))) / 1024u,
		       SpanQuads, (SpanQuads * uint32(sizeof(uint64))) / 1024u, GVoxelPoolDirtyMergeGap);
	}
	// Wave S0 census: this function runs once per applied chunk AND once per
	// unload, so what follows is the per-item cost the batching wave exists to
	// amortise. Clocks gated (GVoxelPoolPushStats), counts unconditional.
	const bool bPushStats = GVoxelPoolPushStats != 0;
	++PushStats.Pushes;

	const double TableCopyStart = bPushStats ? FPlatformTime::Seconds() : 0.0;
	// The chunk table is two small vectors per chunk, so it stays whole.
	TArray<FVector4f> OriginsCopy = ChunkOrigins;
	TArray<FVector4f> ParamsCopy = ChunkParams;
	if (bPushStats)
	{
		PushStats.TableCopyMs += (FPlatformTime::Seconds() - TableCopyStart) * 1000.0;
	}

	// Runs travel with the table because they describe the same thing: which
	// chunk owns which quads. Rebuilt whenever the ALLOCATION set changed, not
	// whenever the table did -- see bRunsDirty. Still not per frame: residency
	// changes a handful of times a second, the camera moves every frame, and only
	// the second of those needs re-culling.
	const double BuildRunsStart = bPushStats ? FPlatformTime::Seconds() : 0.0;
	TArray<FChunkRun> RunsCopy = bTableDirty ? BuildChunkRuns() : TArray<FChunkRun>();
	if (bTableDirty)
	{
		++PushStats.RunsBuilt;
		// The two terms BuildChunkRuns actually costs, kept apart on purpose.
		// AllocationsWalked is what it RESERVES and WALKS -- Allocations is
		// append-only, so this grows with chunks ever added and does not come back
		// down. RunsEmitted is the live set it produces. The gap between them is
		// the finding, if there is one.
		PushStats.AllocationsWalked += int64(Allocations.Num());
		PushStats.RunsEmitted += int64(RunsCopy.Num());
		if (bPushStats)
		{
			PushStats.BuildRunsMs += (FPlatformTime::Seconds() - BuildRunsStart) * 1000.0;
		}
	}

	ENQUEUE_RENDER_COMMAND(VoxelGpuPoolIncrementalUpdate)(
		[Proxy, QuadsSlice = MoveTemp(QuadsSlice), IdsSlice = MoveTemp(IdsSlice),
		 OriginsCopy = MoveTemp(OriginsCopy), ParamsCopy = MoveTemp(ParamsCopy), RunsCopy = MoveTemp(RunsCopy),
		 UploadRuns = MoveTemp(UploadRuns), NewNumQuads, bTableDirty,
		 GpuWrites = MoveTemp(GpuWrites), Buffers = GetOrCreatePoolBuffers(),
		 Name = PoolName](FRHICommandListImmediate& RHICmdList)
	{
		// FIRST, BEFORE THE TWO UPDATES BELOW, AND THAT IS THE ORDERING RULE
		// D1 RESTS ON (Wave D / D1). Those updates are what publish a range as
		// drawable -- UpdateChunkTable_RenderThread hands the cull its runs and
		// UpdateQuadRange_RenderThread advances NumQuads past the high-water
		// mark. Recording the copy passes here means the GPU cannot reach a draw
		// covering a range it has not yet written. Same command, so nothing can
		// get between them.
		if (GpuWrites.Num() > 0)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			VoxelGpuPoolAddWritePasses(GraphBuilder, Buffers, GpuWrites, *Name);
			GraphBuilder.Execute();
		}

		if (bTableDirty)
		{
			Proxy->UpdateChunkTable_RenderThread(RHICmdList, OriginsCopy, ParamsCopy, RunsCopy);
		}
		Proxy->UpdateQuadRange_RenderThread(RHICmdList, QuadsSlice, IdsSlice,
		                                    UploadRuns, NewNumQuads);
	});

	DirtyQuadRanges.Reset();
	bChunkTableDirty = false;
	bRunsDirty = false;

	// Tell the RENDERER the pool grew, not just the component.
	//
	// UpdateBounds() only recomputes this component's cached bounds; the scene
	// keeps whatever bounds it was given when the proxy was created. On the
	// incremental path the proxy is created once and never rebuilt, so without
	// this the renderer culls the pool against the bounds of however few chunks
	// happened to be resident on the first frame -- measured as a 6.4 m box at
	// the player's feet while 2.4 million quads sat in the buffer, drawing
	// nothing because the primitive was frustum-culled almost every frame.
	//
	// MarkRenderTransformDirty, not MarkRenderStateDirty: this pushes bounds
	// and transform to the existing proxy, where the latter would throw the
	// proxy away and rebuild every buffer -- the exact cost the incremental
	// path exists to avoid.
	UpdateBounds();
	MarkRenderTransformDirty();
}

void UVoxelGpuPoolComponent::DestroyRenderState_Concurrent()
{
	LiveProxy = nullptr;
	Super::DestroyRenderState_Concurrent();
}

int32 UVoxelGpuPoolComponent::UpdateChunk(int32 Handle, const TArray<uint64>& InQuads)
{
	if (!Allocations.IsValidIndex(Handle) || !Allocations[Handle].IsValid())
	{
		return INDEX_NONE;
	}

	const FVoxelGpuPoolAllocation Existing = Allocations[Handle];

	// Fits the slot it already has: rewrite in place. This is the case that
	// matters -- an actively dug chunk re-meshes constantly and its quad count
	// barely moves, so free+realloc would churn the allocator for nothing.
	if (uint32(InQuads.Num()) <= Existing.NumQuads)
	{
		const uint32 ChunkId = AllocationChunkIds[Handle];
		for (int32 I = 0; I < InQuads.Num(); ++I)
		{
			PooledQuads[int32(Existing.Offset) + I] = InQuads[I];
			QuadChunkIds[int32(Existing.Offset) + I] = ChunkId;
		}
		// Any tail the chunk no longer needs is hidden rather than left drawing
		// its previous contents.
		for (uint32 I = uint32(InQuads.Num()); I < Existing.NumQuads; ++I)
		{
			QuadChunkIds[int32(Existing.Offset + I)] = kHiddenChunkId;
		}
		MarkQuadsDirty(Existing.Offset, Existing.NumQuads);
		PushUpdatesToProxy();
		return Handle;
	}

	// Outgrew its slot. Reallocate, reusing the chunk's existing table entry so
	// the table does not grow on every edit -- which is why this removal passes
	// bRecycleChunkId=false: the entry is not free, it is about to be re-pointed
	// at the same chunk's new range. Handing it to FreeChunkIds here and then
	// writing quads that reference it would let a LATER AddChunk issue the same
	// id to a different chunk, and two chunks sharing a table entry means one of
	// them silently draws at the other's origin.
	const uint32 ChunkId = AllocationChunkIds[Handle];

	RemoveChunkInternal(Handle, /*bRecycleChunkId=*/false);

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		// The chunk is gone and its entry is now referenced by nothing, so give
		// it back rather than stranding it.
		if (ChunkId != kHiddenChunkId)
		{
			ChunkOrigins[int32(ChunkId)] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			FreeChunkIds.Add(ChunkId);
			bChunkTableDirty = true;
		}
		// Counted with the AddChunk failures, and this is the WORSE of the two:
		// RemoveChunkInternal has already run, so a chunk that was on screen a
		// moment ago is now gone. On a full pool an ordinary re-mesh therefore
		// DELETES existing terrain rather than merely failing to add new terrain,
		// and it does so with no other trace. This path had no log at all.
		++AllocFailureCount;
		AllocFailureQuads += InQuads.Num();
		// The one loud signal -- see MaybeLogSaturation. Fires once, ever, for
		// this pool; every call after the first is a no-op. Doubly worth
		// having here: this is the DESTRUCTIVE failure, not merely a missed add.
		MaybeLogSaturation(/*bAllocFailed=*/true);
		UE_LOG(LogTemp, Warning,
		       TEXT("%s: re-mesh outgrew its slot and the pool is full (%d quads wanted, %u free, "
		            "largest run %u) -- RESIDENT GEOMETRY DROPPED, failure %lld of this run"),
		       *PoolName, InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun(),
		       (long long)AllocFailureCount);
		return INDEX_NONE;
	}
	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}
	Allocations[Handle] = Alloc;
	AllocationChunkIds[Handle] = ChunkId;
	++NumLiveChunks;

	// Resident quads just grew via a realloc; check the 95% trigger the same
	// way AddChunk does. See the comment there -- no-op once latched.
	MaybeLogSaturation(/*bAllocFailed=*/false);

	// The chunk moved. Its table entry is unchanged -- that is the whole point of
	// reusing it -- but the run that names its quads is now wrong, and a stale run
	// does not merely fail to draw this chunk: it points the cull at somebody
	// else's quads.
	bRunsDirty = true;

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	PushUpdatesToProxy();
	return Handle;
}

TArray<UVoxelGpuPoolComponent::FChunkRun> UVoxelGpuPoolComponent::BuildChunkRuns() const
{
	// One entry per LIVE allocation. A freed handle has NumQuads == 0 and its
	// quads already point at the hidden entry, so skipping it here is the same
	// statement the renderer already makes about it -- there is nothing there.
	TArray<FChunkRun> Runs;
	Runs.Reserve(Allocations.Num());
	for (int32 Handle = 0; Handle < Allocations.Num(); ++Handle)
	{
		const FVoxelGpuPoolAllocation& Alloc = Allocations[Handle];
		if (Alloc.NumQuads == 0 || !AllocationChunkIds.IsValidIndex(Handle))
		{
			continue;
		}
		const uint32 ChunkId = AllocationChunkIds[Handle];
		if (ChunkId == kHiddenChunkId)
		{
			continue;
		}
		Runs.Add(FChunkRun{ ChunkId, Alloc.Offset, Alloc.NumQuads });
	}
	// Sorted by pool offset so the proxy can merge neighbours without sorting
	// every frame -- the merge is the difference between one draw per visible
	// chunk and one draw per visible RUN of chunks, and streaming fills the pool
	// in roughly spatial order, so neighbours in the pool are usually neighbours
	// in the world.
	Runs.Sort([](const FChunkRun& A, const FChunkRun& B) { return A.FirstQuad < B.FirstQuad; });
	return Runs;
}

void UVoxelGpuPoolComponent::SetChunkMaterial(UMaterialInterface* InMaterial)
{
	ChunkMaterial = InMaterial;
	MarkRenderStateDirty();
}

UMaterialInterface* UVoxelGpuPoolComponent::GetChunkMaterialOrDefault() const
{
	return ChunkMaterial ? ChunkMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
}

void UVoxelGpuPoolComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
                                              bool bGetDebugMaterials) const
{
	OutMaterials.Add(GetChunkMaterialOrDefault());
}

// ---------------------------------------------------------------------------
// voxel.Stream.PoolClobberTest — the experiment that can actually fail
//
// The clobber this wave fixes is invisible in the happy path: with no GPU
// writer yet, the CPU shadow and the GPU buffer hold identical bytes, so a
// re-upload and a rebind are indistinguishable. A test that merely forces
// proxy recreation and observes "nothing broke" proves nothing at all.
//
// So this makes them distinguishable the only honest way: it DELIBERATELY
// CORRUPTS the CPU shadow, then forces recreation.
//
//   old behaviour (re-upload) -> the corruption reaches the GPU, and the
//                                terrain visibly loses the clobbered quads
//   new behaviour (rebind)    -> the buffer is never rewritten, the corruption
//                                stays on the CPU, and the terrain is unchanged
//
// This is the pool's equivalent of GPUCullDebugSplit: the two hypotheses
// predict visibly different pictures, so the run can come back either way.
//
// DESTRUCTIVE AND DEBUG-ONLY. It leaves the CPU shadow wrong on purpose, so the
// session it runs in is spent — any later edit to a clobbered chunk will write
// from the corrupted shadow. Never leave this in a measurement run.
static void VoxelGpuPoolClobberTest(const TArray<FString>& Args)
{
	const int32 NumToClobber = (Args.Num() > 0) ? FMath::Max(1, FCString::Atoi(*Args[0])) : 200000;

	int32 Found = 0;
	for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
	{
		UVoxelGpuPoolComponent* Comp = *It;
		if (Comp == nullptr || Comp->IsTemplate() || !IsValid(Comp))
		{
			continue;
		}
		++Found;
		Comp->DebugClobberShadowAndRecreate(NumToClobber);
	}

	if (Found == 0)
	{
		UE_LOG(LogTemp, Error,
		       TEXT("voxel.Stream.PoolClobberTest: no live pool component — nothing to test. ")
		       TEXT("Run this after the world has streamed, with voxel.Stream.GPU on."));
	}
}

static FAutoConsoleCommand GVoxelGpuPoolClobberTestCmd(
	TEXT("voxel.Stream.PoolClobberTest"),
	TEXT("DESTRUCTIVE DEBUG. Corrupts the first N quads of the CPU shadow, then forces proxy "
	     "recreation. If the pool buffers are persistent (Wave D / D1) the terrain is UNCHANGED, "
	     "because the corruption never reaches the GPU. If they are re-uploaded, the terrain "
	     "visibly loses those quads. Usage: voxel.Stream.PoolClobberTest [N=200000]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&VoxelGpuPoolClobberTest));

// The full clobber experiment, sequenced on a ticker so the three captures are
// taken in ONE session with nothing else changing between them.
//
// WHY IT TAKES TWO "BEFORE" SHOTS. The noise floor has to come from a same-path
// pair inside this same session, because the measured floor at a settled scene
// is BIMODAL rather than noise-like: 0.00% within a cluster and ~1.81% between
// clusters, from a per-session latch (eye adaptation / sky phase). Assuming a
// fixed threshold would let that latch read as signal. So:
//
//   diff(A, B) = the floor, this session, same path, nothing changed
//   diff(B, C) = the effect, with only the clobber+recreation between them
//
// A result is only meaningful relative to the floor measured beside it.
static void VoxelGpuPoolClobberSession(const TArray<FString>& Args)
{
	const double StartDelay = (Args.Num() > 0) ? FMath::Max(0.0, FCString::Atod(*Args[0])) : 45.0;
	const int32 NumToClobber = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 200000;

	TSharedPtr<double> Elapsed = MakeShared<double>(0.0);
	TSharedPtr<int32> Step = MakeShared<int32>(0);

	UE_LOG(LogTemp, Warning,
	       TEXT("PoolClobberSession queued: settle %.0f s, then shotA, shotB (noise floor pair), "
	            "clobber %d quads, shotC. Screenshots land in Saved/Screenshots."),
	       StartDelay, NumToClobber);

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Elapsed, Step, StartDelay, NumToClobber](float Dt) -> bool
	{
		*Elapsed += double(Dt);
		const double T = *Elapsed;

		const auto Shot = [](const TCHAR* Name)
		{
			FScreenshotRequest::RequestScreenshot(FString(Name), false, false);
		};

		switch (*Step)
		{
		case 0:
			if (T < StartDelay) { return true; }
			UE_LOG(LogTemp, Warning, TEXT("PoolClobberSession: shotA (settled baseline)"));
			Shot(TEXT("clobberA"));
			++*Step;
			return true;
		case 1:
			if (T < StartDelay + 3.0) { return true; }
			UE_LOG(LogTemp, Warning, TEXT("PoolClobberSession: shotB — A/B is the SAME-SESSION noise floor"));
			Shot(TEXT("clobberB"));
			++*Step;
			return true;
		case 2:
			if (T < StartDelay + 6.0) { return true; }
			for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
			{
				if (*It && !It->IsTemplate() && IsValid(*It))
				{
					It->DebugClobberShadowAndRecreate(NumToClobber);
				}
			}
			++*Step;
			return true;
		case 3:
			if (T < StartDelay + 10.0) { return true; }
			UE_LOG(LogTemp, Warning,
			       TEXT("PoolClobberSession: shotC — B/C is the EFFECT. Equal to the A/B floor "
			            "means the buffers were rebound and path 1 is closed."));
			Shot(TEXT("clobberC"));
			++*Step;
			return true;
		default:
			if (T < StartDelay + 14.0) { return true; }
			UE_LOG(LogTemp, Warning, TEXT("PoolClobberSession: done"));
			return false;
		}
	}), 0.0f);
}

static FAutoConsoleCommand GVoxelGpuPoolClobberSessionCmd(
	TEXT("voxel.Stream.PoolClobberSession"),
	TEXT("Runs the whole path-1 clobber experiment in one session: settle, two baseline shots "
	     "(their diff is the same-session noise floor), clobber the CPU shadow, force recreation, "
	     "third shot. Usage: [settleSeconds=45] [quads=200000]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&VoxelGpuPoolClobberSession));

void UVoxelGpuPoolComponent::DebugClobberShadowAndRecreate(int32 NumQuadsToClobber)
{
	const int32 Drawn = int32(Pool.GetHighWaterMark());
	if (Drawn == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s clobber test: pool is empty, nothing to prove"), *PoolName);
		return;
	}

	const int32 N = FMath::Min(NumQuadsToClobber, Drawn);

	// Point them at the hidden chunk (scale 0), which is the pool's own
	// "collapse to a point" encoding — so if this DOES reach the GPU the result
	// is unambiguous missing geometry rather than random garbage that might be
	// mistaken for a shader bug.
	for (int32 I = 0; I < N; ++I)
	{
		QuadChunkIds[I] = kHiddenChunkId;
		PooledQuads[I] = 0;
	}

	UE_LOG(LogTemp, Warning,
	       TEXT("%s clobber test: corrupted the first %d of %d drawn quads in the CPU shadow, "
	            "now forcing proxy recreation. EXPECT: terrain UNCHANGED (buffers rebound). "
	            "A visible hole means the re-upload clobber is still live."),
	       *PoolName, N, Drawn);

	MarkRenderStateDirty();
}

// Lazily allocates the (empty) shared holder. The BUFFERS inside it are created
// on the render thread by the first proxy that runs CreateRenderThreadResources;
// this only guarantees there is somewhere for them to live that outlives any one
// proxy.
FVoxelGpuPoolBuffersRef UVoxelGpuPoolComponent::GetOrCreatePoolBuffers()
{
	if (!PoolBuffers.IsValid())
	{
		PoolBuffers = MakeShared<FVoxelGpuPoolBuffers, ESPMode::ThreadSafe>();
	}
	return PoolBuffers;
}

void UVoxelGpuPoolComponent::BeginDestroy()
{
	// Drop our reference ON THE RENDER THREAD.
	//
	// These are RHI resources whose views were handed to a vertex factory, and
	// the render thread may still be a frame or two behind. Moving the last
	// reference into a render command means the actual release happens in
	// render-thread order behind everything that could still be reading them,
	// rather than wherever the garbage collector happens to run. Getting this
	// wrong does not fail as a compile error or even a visible glitch — it fails
	// as a crash on exit, which is why it is explicit rather than left to the
	// shared pointer's destructor.
	if (PoolBuffers.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VoxelGpuPoolReleaseBuffers)(
			[Buffers = MoveTemp(PoolBuffers)](FRHICommandListImmediate&) mutable
		{
			Buffers.Reset();
		});
		PoolBuffers.Reset();
	}

	Super::BeginDestroy();
}

FPrimitiveSceneProxy* UVoxelGpuPoolComponent::CreateSceneProxy()
{
	if (Pool.GetHighWaterMark() == 0)
	{
		return nullptr;
	}
	// The whole capacity is uploaded, not just the used prefix: incremental
	// writes address absolute pool offsets, so the buffer has to be that big
	// from the start. Only [0, HighWaterMark) is ever drawn.
	TArray<uint64> UsedQuads = PooledQuads;
	TArray<uint32> UsedIds = QuadChunkIds;

	// What is actually about to be drawn, in terms the eye cannot check. A
	// pooled draw has no per-chunk state, so when it renders wrong the only
	// way to tell "the CPU tables are bad" from "the shader reads them wrong"
	// is to print the tables.
	{
		const int32 Drawn = int32(Pool.GetHighWaterMark());
		int32 HiddenQuads = 0, OutOfRangeQuads = 0;
		uint32 MaxIdSeen = 0;
		for (int32 I = 0; I < Drawn; ++I)
		{
			const uint32 Id = UsedIds[I];
			HiddenQuads += (Id == kHiddenChunkId) ? 1 : 0;
			OutOfRangeQuads += (Id >= uint32(ChunkOrigins.Num())) ? 1 : 0;
			MaxIdSeen = FMath::Max(MaxIdSeen, Id);
		}
		UE_LOG(LogTemp, Log,
		       TEXT("%s upload: drawn=%d hidden=%d outOfRange=%d maxId=%u "
		            "tableEntries=%d (%d free) hiddenEntry=(%.1f,%.1f,%.1f,scale=%.3f)"),
		       *PoolName, Drawn, HiddenQuads, OutOfRangeQuads, MaxIdSeen, ChunkOrigins.Num(), FreeChunkIds.Num(),
		       ChunkOrigins[0].X, ChunkOrigins[0].Y, ChunkOrigins[0].Z, ChunkOrigins[0].W);

		// Where the geometry actually is, versus where the renderer will look
		// for it. A pooled draw that renders nothing is nearly always one of
		// these two disagreeing.
		const FBoxSphereBounds WorldBounds = CalcBounds(GetComponentTransform());
		UE_LOG(LogTemp, Log,
		       TEXT("%s placement: comp@(%.0f,%.0f,%.0f) firstChunk=(%.0f,%.0f,%.0f,scale=%.1f) "
		            "boundsOrigin=(%.0f,%.0f,%.0f) boundsExtent=(%.0f,%.0f,%.0f)"),
		       *PoolName, GetComponentLocation().X, GetComponentLocation().Y, GetComponentLocation().Z,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].X : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Y : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Z : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].W : 0.f,
		       WorldBounds.Origin.X, WorldBounds.Origin.Y, WorldBounds.Origin.Z,
		       WorldBounds.BoxExtent.X, WorldBounds.BoxExtent.Y, WorldBounds.BoxExtent.Z);
	}

	FVoxelGpuPoolSceneProxy* Proxy =
		new FVoxelGpuPoolSceneProxy(this, UsedQuads, UsedIds, ChunkOrigins, ChunkParams, BuildChunkRuns(),
		                            PoolName, ChunkTableCapacity, GetOrCreatePoolBuffers());
	LiveProxy = Proxy;
	return Proxy;
}

FBoxSphereBounds UVoxelGpuPoolComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBounds.IsValid)
	{
		return FBoxSphereBounds(FBox(FVector::ZeroVector, FVector(1.0))).TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}
