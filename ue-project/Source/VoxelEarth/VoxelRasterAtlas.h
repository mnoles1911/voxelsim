#pragma once
// FVoxelRasterAtlasCpu -- policy side of the persistent GPU raster atlas
// (-VoxelGpuRasterAtlas; docs/gpu-streaming-architecture.md).
//
// THE COST THIS DELETES, measured: FillRasterWindow samples ~5,800 raster
// pixels (~46 KB) per chunk on the game thread at 0.1-0.5 ms/chunk, ~94% of it
// identical to the lateral neighbour's window and 100% identical to a
// Z-sibling's. At tonight's 2,108 chunks/s that is ~12.2 Mpx/s of game-thread
// re-sampling; at the 50,000/s target it is ~290 Mpx/s -- against a raster
// whose ENTIRE 8.19 km cascade coverage is ~0.3 Mpx at the coarse pitch. The
// atlas uploads each pixel ONCE and re-uploads only what the anchor's movement
// brings into coverage: at 240 m/s that is ~70 pages/s ~= 1.1 Mpx/s, and the
// per-chunk request payload falls from ~46 KB to the ~64 bytes of loose
// constants it always carried. The game-thread sampling term stops scaling
// with chunks/s and starts scaling with ground covered.
//
// SHAPE: one torus of PagesDim x PagesDim pages, kPagePx pixels square each,
// at the world's ONE raster pitch -- no clipmap levels, no compression, no
// allocator, no eviction machinery. Two facts make the single level correct
// and the simplicity safe:
//   * PITCH IS A WORLD PROPERTY, NOT AN LOD. With the fine tier live every
//     chunk at every ring level samples the same 1.875 m raster (see
//     FVoxelWorldImpl::ActiveTiles and amplifier.cpp's isFineTier branch: the
//     two pitches make two entirely different worlds). A near-field-fine /
//     far-field-coarse clipmap would regenerate a far chunk from different
//     pixels than the CPU reference and the collision world use -- the exact
//     cross-arm divergence the ActiveTiles comment forbids. So the atlas holds
//     the whole coverage at world pitch: ~610 MiB at fine pitch under ring 6,
//     ~2.6 MiB at coarse pitch, both logged at init.
//   * RECENTRING IS JUST FILLING. A page that leaves coverage is simply no
//     longer defended; when a page on the opposite edge aliases onto its torus
//     slot the upsert overwrites tag and payload together. A stale tag can
//     never serve wrong data -- a tap checks the tag and a mismatch is a
//     COUNTED miss, never a value.
//
// THE WINDOW RULE IS CONSUMED, NOT COPIED. Every sizing decision here --
// the torus margin at init, the per-request coverage check -- calls
// VoxelGpuRegionBuild::ComputeRasterWindowPx, the single spelling of "which
// pixels can a dispatch tap" that FillRasterWindow and both verify harnesses
// also run. There is no second copy of the arithmetic to drift (the D5
// lesson, recorded at that function).
//
// A MISS CANNOT BE SILENT, BY LAYERED DESIGN:
//   1. PrepareRequest refuses to arm a request whose window (from the one
//      rule) is not fully resident -- the caller falls back to the inline
//      window fill, which is the shipped path, and the decline is COUNTED.
//   2. A tap that misses anyway (the case layer 1 makes "impossible") returns
//      the documented missing-tile answer (sea level / bland climate --
//      exactly vxc::FineTileSampler's missing-block answer) and bumps
//      AtlasMissStats on the GPU.
//   3. The stats window reads that counter back and a non-zero count logs as
//      an ERROR with the first missed pages decoded.
// FAILING READINGS, stated in advance: `gpuMiss>0` is a FAIL (wrong terrain
// was generated and shipped this window; the visible signature is sea-level
// flat ground at the miss site). `served=0` with the atlas armed and the fork
// dispatching is the DEAD reading -- the feature is not running and the leg
// must be read as NOT MEASURED. `inlineFallback` large and not falling after
// warmup means prefetch is broken and the leg is measuring the control path
// wearing the atlas flag.
//
// Plain C++, NOT UHT-parsed; includes voxel-core (the VoxelGpuRegionBuild.h
// doctrine). Game thread only.

#include "CoreMinimal.h"
#include "VoxelGpuRegionBuild.h"        // ComputeRasterWindowPx -- THE window rule
#include "VoxelRasterAtlasGpu.h"

#include "voxelcore/tiles.h"            // vxc::ITileSampler

struct FVoxelGpuRegionRequest;

class FVoxelRasterAtlasCpu
{
public:
	// 128 px * 1.875 m = 240 m of ground per page at fine pitch (3.84 km at
	// coarse). 16,384 px/page = 128 KiB staged per fill at 8 B/px. Small
	// enough that a fill is ~0.3-1.4 ms of sampler time (measured range of
	// FillRasterWindow scaled by pixel count), large enough that steady
	// 240 m/s flight needs only ~70 fills/s.
	static constexpr uint32 kPagePx = 128;

	// Latched -VoxelGpuRasterAtlas=1 (command line, NOT a cvar: -ExecCmds
	// lands after streaming has begun and would silently A/B the same
	// configuration twice -- the project rule).
	static bool Enabled();

	// Game thread, once, before the first Tick. CoverageRadiusMm is the
	// outermost ring's outer radius; MaxCoarseLevel the highest level the
	// cascade streams (the margin probe runs the window rule at EVERY level
	// 0..MaxCoarseLevel and keeps the widest reach -- at level 6 the halo and
	// rep-coordinate terms are 64x wider than level 0's, the 2^L rule).
	// Logs the torus geometry and the payload MiB before allocating it.
	void Init(int64 InPixelSizeMm, int64 CoverageRadiusMm, int32 MaxCoarseLevel);
	bool IsInitialized() const { return PagesDim != 0; }

	FVoxelRasterAtlasGpu& Gpu() { return GpuAtlas; }

	// Game thread, every streaming tick, BEFORE any SubmitGpuMeshJob and
	// before the mesh job manager's Tick -- render commands run in enqueue
	// order, so pages staged here are on the GPU before any dispatch this
	// tick admits against them. Fills toward the anchor's coverage circle,
	// demand-queue first, nearest-first after, under a per-tick time budget;
	// flushes one upsert delta; services the stats window.
	void Tick(vxc::ITileSampler& Tiles, int64 AnchorXMm, int64 AnchorYMm);

	// Game thread, from SubmitGpuMeshJob. Arms Req for atlas sampling if the
	// request's window -- ComputeRasterWindowPx, the same rule the inline
	// fill runs -- is fully resident. Returns false (and queues the missing
	// pages at demand priority, and counts the decline) if not: the caller
	// MUST then fall back to the inline FillRasterWindow path, which stays
	// correct in every state.
	bool PrepareRequest(FVoxelGpuRegionRequest& Req);

	// PIE teardown: enqueues the GPU-side release. Safe to call once, after
	// the last Tick.
	void Shutdown();

	// --- GATE-ONLY (voxel.GPU.VerifyRasterAtlas) ---------------------------
	// Production never calls these; they exist so the gate can prove BOTH
	// directions -- that a covered window byte-matches the inline path, and
	// that an uncovered one is refused AND counted rather than silently
	// clamped.

	// Fills every page of Req's window (through the one rule) immediately,
	// ignoring the tick budget, and flushes the delta. Lets a scratch atlas
	// cover a fixture without a streaming anchor or a budgeted warmup.
	void FillWindowNow(vxc::ITileSampler& Tiles, const FVoxelGpuRegionRequest& Req);

	// Drops one page from the host mirror AND the GPU tags (invalidate delta,
	// flushed immediately). The gate drops a page under a fixture and then
	// demands that PrepareRequest refuses (layer 1) and that a force-armed
	// dispatch counts misses (layer 2) -- the counter proven able to fire.
	void DebugDropPage(int64 PageX, int64 PageY);

private:
	struct FPagePt { int64 X = 0; int64 Y = 0; };

	uint32 SlotOf(int64 PageX, int64 PageY) const;
	static uint32 PackTag(int64 PageX, int64 PageY);
	bool IsPageResident(int64 PageX, int64 PageY) const;
	void FillPage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY);
	void LogWindowIfDue();

	int64 PixelSizeMm = 0;
	int32 MaxLevel = 0;
	uint32 PagesDim = 0;
	// Coverage circle in pages (Chebyshev), margin already folded in.
	int64 RadiusPages = 0;

	// Host mirror of the GPU tag table: what page each torus slot will hold
	// once every flushed delta lands. Authoritative for PrepareRequest -- the
	// GPU can only ever be AHEAD of a reader within the same tick, never
	// behind, because deltas flush before dispatches enqueue.
	TArray<uint32> SlotTags;

	// Pages requested by a declined PrepareRequest -- filled before the sweep,
	// because a demand page is blocking a real chunk right now.
	TArray<FPagePt> DemandQueue;
	TSet<uint64> DemandQueued;

	FVoxelRasterAtlasGpuDelta PendingDelta;
	FVoxelRasterAtlasGpu GpuAtlas;

	// Stats window.
	double LastWindowLogSeconds = 0.0;
	uint64 Served = 0;
	uint64 DeclinedCold = 0;
	uint64 PagesFilled = 0;
	uint64 FillMicroseconds = 0;
	uint64 BytesUploaded = 0;
	uint64 GpuMissLifetime = 0;
	bool bLoggedPitchMismatch = false;
};
