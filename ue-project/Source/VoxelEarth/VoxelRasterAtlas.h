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

#include "Tasks/Task.h"                 // UE::Tasks::TTask -- the off-thread fill (mode 3)

#include "voxelcore/tiles.h"            // vxc::ITileSampler

#include <atomic>

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

	// Drops one page from the host mirror AND the GPU tags (invalidate delta,
	// flushed immediately). The gate drops a page under a fixture and then
	// demands that PrepareRequest refuses (layer 1) and that a force-armed
	// dispatch counts misses (layer 2) -- the counter proven able to fire.
	void DebugDropPage(int64 PageX, int64 PageY);

	// --- THE WARMUP PROBLEM, AND THE FILL-MODE LADDER ----------------------
	//
	// MEASURED (Saved/q-L4ship.log, the leg's first five 5 s windows):
	//
	//   fills=205 (553.1 ms GT)  fills=280 (878.0)  fills=272 (812.6)
	//   fills=286 (1225.6)       fills=478 (834.5)  -> then fills=0 forever
	//
	//   1,521 pages, 4,303.8 ms of GAME THREAD, 2.83 ms/page.
	//
	// 4.3 s of a ~21-24 s cold start, on the thread that is the constraint, and
	// it is a FIXED cost set by world extent: 1,521 pages is the whole coverage
	// disc and the sweep finishes it exactly once. It does not shrink when
	// chunks/s rises, so every throughput fix makes it a LARGER share of what
	// remains -- at 50,000 chunks/s a 3.3 s settle would be mostly this.
	//
	// IT IS NOT WASTE, and the arithmetic matters before anyone proposes
	// deleting it: the inline FillRasterWindow it replaces samples ~5,800 px
	// per chunk, so 164,733 chunks is ~955 Mpx against the atlas's
	// 1,521 x 16,384 = 24.9 Mpx. The atlas is already 38x cheaper. The question
	// is not whether to pay it but WHERE, and HOW MUCH.
	//
	// WHERE THE 2.83 ms GOES is answered by the fill-cost buckets this class
	// now prints on every window line (see FPageFillCost). They are timed per
	// PHASE, not per pixel -- six FPlatformTime::Cycles64 reads per page
	// against 32,768 sampler calls, so the instrument is ~0.005% of what it
	// measures rather than a distortion of it -- and the RESIDUAL is printed,
	// so a bucket set that stops covering the work shows up as a growing
	// unnamed term instead of as buckets that quietly stop adding up.
	//
	// -VoxelGpuRasterAtlasFill=N, latched, default 0. One rung per arm, in the
	// order the ladder lifts:
	//
	//   0  TODAY. Budgeted synchronous sweep over the Chebyshev SQUARE, every
	//      pixel of every page sampled individually. The control -- and it
	//      still prints the buckets, so ONE leg answers "where does the
	//      2.83 ms go".
	//   1  DISC, NOT SQUARE. The sweep visits the 39x39 page square while
	//      coverage is a 4.10 km + margin DISC (AdmitOuterUU is a radius). At
	//      the shipped geometry (CoverageRadiusPx=2,237, RadiusPages=19) the
	//      disc admits 1,021-1,036 of the 1,521 pages depending on where in its
	//      page the anchor sits: the corners are ~32% of the sweep and no chunk
	//      ever asks for them. At 2.83 ms/page that is ~1.37 s on its own.
	//      A page is skipped when its NEAREST pixel is outside CoverageRadiusPx
	//      -- the radius Init derived the margin from, not a second spelling.
	//      Skipping too much cannot generate wrong terrain: it costs a
	//      PrepareRequest decline, which demand-queues the page and takes the
	//      inline path, so `inlineFallback` is the reading that says the disc
	//      is too tight.
	//   2  1 + CLIMATE DEDUP. The fine tier carries no climate plane, so every
	//      climate() lands in the COARSE sampler at 30,000 mm/px: at the fine
	//      1,875 mm/px pitch one coarse cell covers 16x16 = 256 atlas pixels.
	//      A page needs 8x8 = 64 distinct climate samples and today takes
	//      16,384 -- 255 of every 256 calls return a value already in hand.
	//      Requires the coarse pitch (-VoxelGpuRasterAtlasClimatePitchMm=30000,
	//      the `scale=1` the tile-grid line already prints); with it unset the
	//      mode is INERT and the window line says `climateDedup=UNAVAILABLE`,
	//      which is a different reading from `climateDedup=on saved=0`.
	//   3  2 + OFF THE GAME THREAD. The sampling runs on UE::Tasks workers into
	//      slot buffers; the game thread only harvests finished slots, memcpys
	//      them into the delta and flips the mirror. This REMOVES the term
	//      rather than reducing it -- and it carries a hazard the other modes
	//      do not; see AsyncFillTasks() in the .cpp before arming it.
	//
	// THE REGISTERED DISPROOF, written before any of it was measured: mode 3
	// must move `[raster-atlas] window: ... ms GT` to ~0 (with `asyncPages` > 0
	// proving it ran) AND cut cold settle by roughly the game-thread time it
	// removed -- about 4.3 s against mode 0, about 2 s on top of mode 2. If the
	// GT time vanishes and settle does not move by at least half of what was
	// removed, the atlas fill was NOT on the critical path: modes 1-3 are then
	// tuning a term nobody waits for, and the right answer is to revert them
	// and write down why, not to tune them further.

	// --- GATE-ONLY (voxel.GPU.VerifyRasterAtlas) ---------------------------
	// Production never calls these; they exist so the gate can prove BOTH
	// directions -- that a covered window byte-matches the inline path, and
	// that an uncovered one is refused AND counted rather than silently
	// clamped.

	// Fills every page of Req's window (through the one rule) immediately,
	// ignoring the tick budget, and flushes the delta. Lets a scratch atlas
	// cover a fixture without a streaming anchor or a budgeted warmup.
	void FillWindowNow(vxc::ITileSampler& Tiles, const FVoxelGpuRegionRequest& Req);

	// Latched fill mode, above. Public so a gate or a log line can state which
	// ladder rung a run is on without re-parsing the command line.
	static int32 FillMode();

private:
	struct FPagePt { int64 X = 0; int64 Y = 0; };

	// WHERE A PAGE'S TIME GOES -- named buckets with an explicit residual.
	//
	// THE RULE THIS OBEYS: a bucket set that does not sum to the measured total
	// is not a breakdown, it is a guess with decoration. TotalCycles brackets
	// the whole of SamplePage; Elev/Climate/Stage bracket its three phases; the
	// window line prints `resid=` as Total - (Elev + Climate + Stage), in ms
	// AND as a percentage, so a phase that stops being covered (someone adds
	// work, or a mode skips one) shows as a residual that GROWS.
	//
	// TRAFFIC BEFORE TIMING, and the two failures print differently:
	//   * `pages=N elevCalls=0` -- the elevation phase never ran. The
	//     instrument is outside the path, or the phase was skipped; the line
	//     says PHASE-NEVER-RAN in words.
	//   * `elevCalls=M elevMs=0.0` -- the phase ran and the TIMER is dead.
	//     Different words, different fix.
	// Neither can be confused with `pages=0`, which is the atlas not filling at
	// all -- steady state, and the healthy reading after warmup.
	struct FPageFillCost
	{
		uint64 TotalCycles = 0;
		uint64 ElevCycles = 0;
		uint64 ClimateCycles = 0;
		uint64 StageCycles = 0;
		uint64 ElevCalls = 0;
		uint64 ClimateCalls = 0;   // sampler calls actually made (post-dedup)
		uint64 ClimatePixels = 0;  // pixels that needed a value (pre-dedup)
		uint64 AuditChecked = 0;
		uint64 AuditMismatch = 0;
		uint64 Pages = 0;

		void Add(const FPageFillCost& O)
		{
			TotalCycles += O.TotalCycles;      ElevCycles += O.ElevCycles;
			ClimateCycles += O.ClimateCycles;  StageCycles += O.StageCycles;
			ElevCalls += O.ElevCalls;          ClimateCalls += O.ClimateCalls;
			ClimatePixels += O.ClimatePixels;  AuditChecked += O.AuditChecked;
			AuditMismatch += O.AuditMismatch;  Pages += O.Pages;
		}
	};

	// One batch of pages handed to one worker. Ownership passes GAME THREAD ->
	// WORKER at launch and WORKER -> GAME THREAD when bDone flips, and nothing
	// but bDone is touched by both: that single release/acquire pair is the
	// whole synchronisation argument, and it is why a slot owns its own buffers
	// instead of writing into PendingDelta (which the game thread appends to
	// and flushes in the same tick a worker would be writing it).
	struct FFillSlot
	{
		TArray<FPagePt> Pages;
		TArray<int32> Elev;
		TArray<uint32> Climate;
		FPageFillCost Cost;
		std::atomic<bool> bDone{false};
		bool bBusy = false;              // game thread only
		UE::Tasks::TTask<void> Task;
	};

	static uint32 PackTag(int64 PageX, int64 PageY);
	static uint64 PageKey(int64 PageX, int64 PageY);
	// The ONE spelling of the atlas's climate word. Both the fill and the dedup
	// audit's compare go through it, so a re-ordered field cannot make the
	// audit disagree with the payload it is auditing.
	static uint32 PackClimate(const vxc::ClimateSample& C);

	uint32 SlotOf(int64 PageX, int64 PageY) const;
	bool IsPageResident(int64 PageX, int64 PageY) const;

	// Is this page inside the coverage DISC (mode >= 1)? Derived from
	// CoverageRadiusPx, which Init computed once from the same
	// ComputeRasterWindowPx margin probe the whole class is built on.
	bool IsPageInCoverage(int64 PageX, int64 PageY, int64 AnchorPxX, int64 AnchorPxY) const;

	// The ONE fill-order policy, used by the synchronous and the asynchronous
	// paths alike so the two arms of an A/B cannot differ in WHICH pages they
	// choose -- only in where the sampling runs. Demand queue first (each entry
	// is blocking a chunk that already asked), then nearest-first Chebyshev
	// rings over the coverage disc. Returns false when there is nothing left to
	// fill. Never returns a page that is resident or already in flight.
	bool NextPageToFill(int64 AnchorPxX, int64 AnchorPxY, FPagePt& Out, bool& bOutFromDemand);

	// Samples one page into caller-owned buffers (kPagePx*kPagePx each) and
	// accumulates the phase buckets. PURE with respect to this object except
	// for Cost -- no mirror, no delta -- which is what makes it callable from a
	// worker.
	void SamplePage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY,
	                int32* OutElevation, uint32* OutClimate, FPageFillCost& Cost) const;

	// Synchronous fill: SamplePage straight into PendingDelta, then flip the
	// mirror. The game-thread path (modes 0-2) and the gate's FillWindowNow.
	void FillPage(vxc::ITileSampler& Tiles, int64 PageX, int64 PageY);

	// Appends one page's worth of uninitialised pixels to the pending delta and
	// returns their base index; its partner writes the page's meta and flips
	// the mirror. Split so that "a page becomes resident" has ONE spelling
	// shared by the synchronous fill and the asynchronous harvest -- the two
	// paths differ in where the pixels came from and in nothing else.
	int32 StagePageStorage();
	void CommitStagedPage(int64 PageX, int64 PageY, int32 Base);

	void PopDemandFront();

	// --- mode 3 -------------------------------------------------------------
	void HarvestAsyncSlots();
	void LaunchAsyncSlots(vxc::ITileSampler& Tiles, int64 AnchorPxX, int64 AnchorPxY);
	void WaitForAsyncSlots();

	// Game thread. Turns a worker-counted audit mismatch into the one log line
	// and the one latch. Workers only COUNT; this decides.
	void ReactToClimateAudit();

	void LogWindowIfDue();

	int64 PixelSizeMm = 0;
	int32 MaxLevel = 0;
	uint32 PagesDim = 0;
	// Coverage circle in pages (Chebyshev), margin already folded in.
	int64 RadiusPages = 0;
	// The SAME radius RadiusPages was rounded up from, kept in pixels so the
	// disc test (mode >= 1) IS the coverage rule rather than a restatement.
	int64 CoverageRadiusPx = 0;

	// Fine pixels per coarse climate cell, from
	// -VoxelGpuRasterAtlasClimatePitchMm / PixelSizeMm. 0 = "the host never
	// told us the climate pitch", which is a DIFFERENT state from 1 ("told us,
	// and it buys nothing"), and the window line distinguishes them.
	int64 ClimateRunPx = 0;
	// Latched off after an audit mismatch: the dedup is then wrong about this
	// world and must not write another pixel.
	// ATOMIC because a worker reads it inside SamplePage while the game thread
	// may be setting it -- relaxed is enough: a worker that sees the old value
	// simply dedups one more page, and the audit catches that page too.
	std::atomic<bool> bClimateDedupDisabled{false};

	// Host mirror of the GPU tag table: what page each torus slot will hold
	// once every flushed delta lands. Authoritative for PrepareRequest -- the
	// GPU can only ever be AHEAD of a reader within the same tick, never
	// behind, because deltas flush before dispatches enqueue.
	TArray<uint32> SlotTags;

	// Pages requested by a declined PrepareRequest -- filled before the sweep,
	// because a demand page is blocking a real chunk right now.
	TArray<FPagePt> DemandQueue;
	TSet<uint64> DemandQueued;

	// Pages handed to a worker and not yet harvested. NOT resident (the mirror
	// still reads sentinel, so PrepareRequest still declines them and the chunk
	// still takes the correct inline path), but they must not be selected
	// twice.
	TSet<uint64> PagesInFlight;
	TArray<TUniquePtr<FFillSlot>> FillSlots;

	// SWEEP CURSOR -- the ring the last search found work in. Pages only ever
	// become resident during a warmup, so resuming from there instead of from
	// R=0 turns each search from O(disc) into O(ring perimeter). Mode 3 asks up
	// to tasks x batch times per tick, and an O(1,521)-lookup scan per ask
	// would have put ~0.7 ms/tick of pure searching on the game thread -- the
	// instrument becoming the cost. Reset whenever the anchor's page moves, and
	// retried from 0 whenever a resumed scan finds nothing, so it can never
	// permanently miss a hole.
	int64 SweepResumeR = 0;
	int64 SweepCenterPageX = MIN_int64;
	int64 SweepCenterPageY = MIN_int64;

	FVoxelRasterAtlasGpuDelta PendingDelta;
	FVoxelRasterAtlasGpu GpuAtlas;

	// Stats window.
	double LastWindowLogSeconds = 0.0;
	uint64 Served = 0;
	uint64 DeclinedCold = 0;
	uint64 PagesFilled = 0;
	uint64 PagesFilledLifetime = 0;
	uint64 BytesUploaded = 0;
	uint64 GpuMissLifetime = 0;
	uint64 AsyncPagesHarvested = 0;
	uint64 AsyncLaunches = 0;
	uint64 PrimeCalls = 0;
	// THE OTHER HALF OF THE GAME-THREAD RASTER COST, and it was never split.
	//
	// The submit path's `raster` bucket (VoxelWorldSubsystem's SubT3 - SubT2)
	// measured 2,856.7 ms across the cold start of Saved/q-L4ship.log -- on top
	// of this class's own 4,303.8 ms of page fill, so the two raster terms
	// together are ~7.2 s of a ~21-24 s settle. But that bucket brackets TWO
	// things: PrepareRequest's coverage walk, which every served chunk pays,
	// and the inline FillRasterWindow every DECLINED chunk pays. 114,169 serves
	// and 2,354 declines in the same span, so which of them the 2.86 s belongs
	// to changes the answer completely -- and nothing measured it.
	//
	// This pair does. `raster` minus `prepareMs` is the inline-fill half, and
	// it is a per-decline cost that a faster warmup deletes rather than moves.
	// Two Cycles64 reads per PrepareRequest: ~40 ns against a call that walks
	// the window's pages, ~0.05% of the bucket at today's rate and ~0.2% at
	// 50,000 chunks/s -- stated rather than assumed away.
	uint64 PrepareCycles = 0;
	uint64 PrepareCalls = 0;
	// GAME-THREAD cycles spent in Tick's fill section and nowhere else. THE
	// number the registered disproof is written against, and deliberately NOT
	// the sum of the phase buckets: in mode 3 those buckets are WORKER time,
	// and adding worker milliseconds into a game-thread total is exactly the
	// mistake that made a 99%-contention reading worth -0.8% of throughput.
	uint64 GtFillCycles = 0;
	int64 LastAnchorPxX = 0;
	int64 LastAnchorPxY = 0;
	FPageFillCost WindowCost;
	bool bLoggedPitchMismatch = false;
};
